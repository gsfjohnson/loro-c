// Exercises non-trivial LoroDoc methods beyond the M1 smoke path:
//   - peer_id / set_peer_id
//   - commit and pending-txn tracking
//   - state_vv / state_frontiers / vv_to_frontiers / frontiers_to_vv
//   - export/import variants (snapshot, snapshot_at, updates, json)
//   - fork / fork_at
//   - get_deep_value
//   - has_container
//
// Subscriptions are exercised by test_subscriptions.cpp.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

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

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
