// Exercises non-trivial LoroDoc methods beyond the M1 smoke path:
//   - peer_id / set_peer_id
//   - commit and pending-txn tracking
//   - state_vv / state_frontiers / vv_to_frontiers / frontiers_to_vv
//   - export/import variants (snapshot, snapshot_at, updates, json)
//   - fork / fork_at
//   - revert_to
//   - get_deep_value
//   - has_container
//
// Subscriptions are exercised by test_subscriptions.cpp.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run_loro_1_17();

bool run() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(42);
    if (doc->peer_id() != 42) return fail("peer_id != 42 after set");

    auto text = doc->get_text(root("body"));
    text->insert(0, "alpha");

    if (doc->get_pending_txn_len() == 0) {
        return fail("pending txn should be non-empty before commit");
    }
    doc->commit();
    if (doc->get_pending_txn_len() != 0) {
        return fail("pending txn should be empty after commit");
    }

    auto frontiers_after_alpha = doc->state_frontiers();
    auto vv_after_alpha = doc->state_vv();

    auto vv_round_trip = doc->frontiers_to_vv(frontiers_after_alpha);
    if (!vv_round_trip->eq(vv_after_alpha)) {
        return fail("frontiers_to_vv round-trip mismatch");
    }
    auto frontiers_round_trip = doc->vv_to_frontiers(vv_after_alpha);
    if (!frontiers_round_trip->eq(frontiers_after_alpha)) {
        return fail("vv_to_frontiers round-trip mismatch");
    }

    text->insert(text->len_unicode(), " beta");
    doc->commit();

    auto snapshot_now = doc->export_snapshot();
    auto snapshot_at_alpha = doc->export_snapshot_at(frontiers_after_alpha);

    auto restored_alpha = loro::LoroDoc::init();
    restored_alpha->import(snapshot_at_alpha);
    if (restored_alpha->get_text(root("body"))->to_string() != "alpha") {
        return fail("export_snapshot_at(alpha) did not restore alpha-only state");
    }

    auto restored_now = loro::LoroDoc::init();
    restored_now->import(snapshot_now);
    if (restored_now->get_text(root("body"))->to_string() != "alpha beta") {
        return fail("export_snapshot did not restore latest state");
    }

    auto forked = doc->fork();
    if (forked->peer_id() == doc->peer_id()) {
        return fail("fork should produce a different peer id");
    }
    if (forked->get_text(root("body"))->to_string() != "alpha beta") {
        return fail("fork did not preserve content");
    }

    auto forked_at_alpha = doc->fork_at(frontiers_after_alpha);
    if (forked_at_alpha->get_text(root("body"))->to_string() != "alpha") {
        return fail("fork_at(alpha) did not produce alpha-only state");
    }

    auto deep = doc->get_deep_value();
    auto *deep_map = std::get_if<loro::LoroValue::kMap>(&deep.get_variant());
    if (!deep_map) return fail("get_deep_value should return a map");
    if (deep_map->value->find("body") == deep_map->value->end()) {
        return fail("deep value missing root container 'body'");
    }

    auto cid = loro::ContainerId(loro::ContainerId::kRoot{
        "body", loro::ContainerType(loro::ContainerType::kText{})});
    if (!doc->has_container(cid)) {
        return fail("has_container should be true for created root text");
    }

    auto json = doc->export_json_updates(loro::VersionVector::init(),
                                         doc->state_vv());
    if (json.empty()) return fail("export_json_updates returned empty");

    auto reimported = loro::LoroDoc::init();
    reimported->import_json_updates(json);
    if (reimported->get_text(root("body"))->to_string() != "alpha beta") {
        return fail("import_json_updates round-trip failed");
    }

    auto updates = doc->export_updates(loro::VersionVector::init());
    auto from_updates = loro::LoroDoc::init();
    from_updates->import(updates);
    if (from_updates->get_text(root("body"))->to_string() != "alpha beta") {
        return fail("export_updates round-trip failed");
    }

    // --- oplog introspection / commit origin (UPVERT_3) ---
    if (doc->len_ops() == 0) return fail("len_ops should be > 0 after committed edits");
    // A live doc (never checked out) is attached, not detached.
    if (doc->is_detached()) return fail("a live doc should not be detached");

    auto oplog_fr = doc->oplog_frontiers();
    if (oplog_fr->to_vec().empty()) {
        return fail("oplog_frontiers should be non-empty after commits");
    }

    auto ops_before = doc->len_ops();
    doc->set_next_commit_origin("unit-test");  // attached to the next commit; must not throw
    text->insert(text->len_unicode(), "!");
    doc->commit();
    if (doc->len_ops() <= ops_before) {
        return fail("len_ops should grow after another committed op");
    }

    // --- revert_to: history-preserving rewind to an earlier frontiers ---
    // The body currently reads "alpha beta!"; revert back to the alpha frontiers.
    auto ops_before_revert = doc->len_ops();
    doc->revert_to(frontiers_after_alpha);
    if (doc->get_text(root("body"))->to_string() != "alpha") {
        return fail("revert_to(alpha) did not restore alpha-only state");
    }
    // Unlike checkout, revert_to keeps the doc attached...
    if (doc->is_detached()) {
        return fail("revert_to should leave the doc attached, not detached");
    }
    // ...and records the inverse operations as a new change (history grows).
    if (doc->len_ops() <= ops_before_revert) {
        return fail("revert_to should record inverse ops, growing len_ops");
    }

    return run_loro_1_17();
}

// LoroDoc methods added with loro 1.17 (not in loro-cpp): export_pending_updates,
// import_*_with_history_mode and the decoded value cache limit.
bool run_loro_1_17() {
    // Updates waiting for a missing dependency survive a snapshot restore only through
    // export_pending_updates (the example on loro's LoroDoc::export_pending_updates).
    auto source = loro::LoroDoc::init();
    source->set_peer_id(1);
    auto text = source->get_text(root("text"));
    text->insert(0, "base|");
    source->commit();
    auto base = source->export_snapshot();
    auto before_a = source->oplog_vv();
    text->insert(5, "A");
    source->commit();
    auto a = source->export_updates(before_a);
    auto before_b = source->oplog_vv();
    text->insert(6, "B");
    source->commit();
    auto b = source->export_updates(before_b);

    auto receiver = loro::LoroDoc::init();
    receiver->import(base);
    if (receiver->export_pending_updates()) return fail("nothing should be pending yet");
    receiver->import(b);
    auto snapshot = receiver->export_snapshot();
    auto pending = receiver->export_pending_updates();
    if (!pending || pending->empty()) return fail("B should be pending");

    auto restored = loro::LoroDoc::init();
    restored->import(snapshot);
    restored->import(*pending);
    auto restored_text = restored->get_text(root("text"));
    if (restored_text->to_string() != "base|") return fail("B applied before A arrived");
    if (!restored->export_pending_updates()) return fail("restored doc lost the pending B");
    restored->import(a);
    if (restored_text->to_string() != "base|AB") return fail("A did not release the pending B");
    if (restored->export_pending_updates()) return fail("nothing should be pending after A");

    // Binary that went through JSON updates comes back as a list of numbers. Passing that
    // history on as binary is rejected by an exact import and accepted by kJsonLossy.
    auto writer = loro::LoroDoc::init();
    writer->set_peer_id(1);
    writer->get_map(root("m"))->insert(
        "bin", std::make_shared<PrimitiveValue>(
                   loro::LoroValue(loro::LoroValue::kBinary{{0, 1, 255}})));
    writer->commit();
    auto prefix = writer->export_updates(loro::VersionVector::init());
    auto relay = loro::LoroDoc::init();
    relay->set_peer_id(2);
    relay->import_json_updates(
        writer->export_json_updates(loro::VersionVector::init(), writer->oplog_vv()));
    relay->get_map(root("relay"))->insert("extra", i64_value(1));
    relay->commit();
    auto blob = relay->export_updates(loro::VersionVector::init());

    auto exact = loro::LoroDoc::init();
    exact->import(prefix);
    bool rejected = false;
    try {
        exact->import(blob);
    } catch (const loro::LoroError &) {
        rejected = true;
    }
    if (!rejected) return fail("exact import of JSON-relayed binary should be rejected");
    if (exact->get_map(root("relay"))->len() != 0) return fail("rejected import changed doc");

    auto lossy = loro::LoroDoc::init();
    lossy->import(prefix);
    lossy->import_with_history_mode(blob, "relay", loro::ImportHistoryMode::kJsonLossy);
    if (lossy->get_map(root("relay"))->len() != 1) return fail("kJsonLossy import failed");
    auto kept = lossy->get_map(root("m"))->get("bin")->as_value();
    if (!kept || !std::holds_alternative<loro::LoroValue::kBinary>(kept->get_variant())) {
        return fail("kJsonLossy import should keep the receiver's binary value");
    }

    auto batch = loro::LoroDoc::init();
    batch->import(prefix);
    batch->import_batch_with_history_mode({blob}, loro::ImportHistoryMode::kJsonLossy);
    if (batch->get_map(root("relay"))->len() != 1) return fail("kJsonLossy batch import failed");

    auto cached = loro::LoroDoc::init();
    if (cached->decoded_value_cache_limit() != 2048) return fail("default cache limit != 2048");
    cached->set_decoded_value_cache_limit(16);
    if (cached->fork()->decoded_value_cache_limit() != 16) return fail("fork lost cache limit");
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
