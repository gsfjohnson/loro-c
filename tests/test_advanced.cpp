// M4: advanced surface via the C++ wrapper.
//   - VersionVector / Frontiers: encode/decode, includes, compare, delta sync
//   - time travel: checkout to an earlier frontiers and back
//   - FractionalIndex: between / ordering / string & bytes round trip
//   - Awareness + EphemeralStore (+ change subscription)
//   - UndoManager: undo/redo + on_push/on_pop metadata round trip
//   - JSONPath: value + container queries
//   - commit hooks: set_next_commit_message, pre-commit, first-commit-from-peer
//   - travel_change_ancestors

#include <loro/loro.hpp>

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// Two docs converge by shipping only the delta computed from a version vector.
static void test_version_vector_sync() {
    loro::Doc a;
    loro::Text ta = a.get_text("t");
    ta.insert(0, "hello");
    a.commit();

    loro::Doc b;
    // b is empty: the delta from b's (empty) vv brings it fully up to date.
    loro::VersionVector b_vv = b.state_vv();
    std::vector<std::uint8_t> delta = a.export_updates_from(b_vv);
    CHECK(!delta.empty());
    b.import(delta);
    CHECK(b.get_text("t").to_string() == "hello");

    // a now includes everything b has, but not vice-versa.
    CHECK(a.oplog_vv().includes(b.oplog_vv()));

    // VersionVector encode/decode round trips and compares equal.
    std::vector<std::uint8_t> enc = a.oplog_vv().encode();
    loro::VersionVector decoded = loro::VersionVector::decode(enc);
    auto cmp = a.oplog_vv().compare(decoded);
    CHECK(cmp.has_value() && *cmp == 0);
}

// Frontiers encode/decode and round trip through the document.
static void test_frontiers() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "abc");
    doc.commit();

    loro::Frontiers f = doc.oplog_frontiers();
    CHECK(f.size() == 1);
    std::vector<loro::Id> ids = f.to_vec();
    CHECK(ids.size() == 1);

    // encode/decode round trips.
    loro::Frontiers decoded = loro::Frontiers::decode(f.encode());
    CHECK(decoded.size() == 1);
    CHECK(decoded.contains(ids[0]));

    // vv <-> frontiers conversions agree.
    loro::VersionVector vv = doc.frontiers_to_vv(f);
    loro::Frontiers back = doc.vv_to_frontiers(vv);
    CHECK(back.contains(ids[0]));
}

// Checkout to an earlier version, then back to latest.
static void test_time_travel() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    t.insert(0, "v1");
    doc.commit();
    loro::Frontiers v1 = doc.oplog_frontiers();

    t.insert(2, "-v2");
    doc.commit();
    CHECK(t.to_string() == "v1-v2");

    doc.checkout(v1);
    CHECK(doc.is_detached());
    CHECK(doc.get_text("t").to_string() == "v1");

    doc.checkout_to_latest();
    CHECK(!doc.is_detached());
    CHECK(doc.get_text("t").to_string() == "v1-v2");
}

// FractionalIndex: a < c < b, with string/bytes round trips preserving order.
static void test_fractional_index() {
    loro::FractionalIndex a = loro::FractionalIndex::create();
    auto b_opt = loro::FractionalIndex::between(&a, nullptr);
    CHECK(b_opt.has_value());
    loro::FractionalIndex b = std::move(*b_opt);
    CHECK(b.compare(a) > 0);

    auto c_opt = loro::FractionalIndex::between(&a, &b);
    CHECK(c_opt.has_value());
    loro::FractionalIndex c = std::move(*c_opt);
    CHECK(c.compare(a) > 0);
    CHECK(c.compare(b) < 0);

    // Round trips preserve identity.
    loro::FractionalIndex from_str = loro::FractionalIndex::from_string(b.to_string());
    CHECK(from_str.compare(b) == 0);
    loro::FractionalIndex from_bytes = loro::FractionalIndex::from_bytes(b.to_bytes());
    CHECK(from_bytes.compare(b) == 0);
}

// Awareness: encode local state on one side, apply on the other.
static void test_awareness() {
    loro::Awareness a(1, 30000);
    loro::Awareness b(2, 30000);
    a.set_local_state("{\"name\":\"alice\"}");

    std::vector<std::uint8_t> enc = a.encode_all();
    b.apply(enc);

    std::string states = b.get_all_states();
    CHECK(states.find("alice") != std::string::npos);
    CHECK(a.peer() == 1);
}

// EphemeralStore: set/get, change subscription, and apply between two stores.
static void test_ephemeral_store() {
    loro::EphemeralStore store(30000);

    int calls = 0;
    std::string added;
    auto sub = store.subscribe([&](const loro::EphemeralStoreEvent& ev) {
        ++calls;
        added = ev.added();
    });

    store.set("cursor", "42");
    CHECK(calls >= 1);
    CHECK(added.find("cursor") != std::string::npos);

    auto v = store.get("cursor");
    CHECK(v.has_value() && *v == "42");
    CHECK(store.keys().find("cursor") != std::string::npos);

    // Sync to a second store.
    loro::EphemeralStore other(30000);
    other.apply(store.encode_all());
    auto v2 = other.get("cursor");
    CHECK(v2.has_value() && *v2 == "42");

    store.remove("cursor");
    CHECK(!store.get("cursor").has_value());
}

// UndoManager: undo/redo plus on_push/on_pop metadata round trip.
static void test_undo_manager() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    loro::UndoManager um = doc.undo_manager();

    int pushes = 0;
    std::string popped_meta;
    um.set_on_push([&](loro::UndoOrRedo, loro::CounterSpan, const loro::DiffEvent*,
                       loro::UndoMeta& meta) {
        ++pushes;
        meta.set_value("\"tag\"");
    });
    um.set_on_pop([&](loro::UndoOrRedo, loro::CounterSpan, const loro::UndoMeta& meta) {
        popped_meta = meta.value();
    });

    t.insert(0, "hello");
    doc.commit();
    CHECK(pushes >= 1);
    CHECK(um.can_undo());

    CHECK(um.undo());
    CHECK(doc.get_text("t").to_string().empty());
    // The metadata stored on push came back on pop.
    CHECK(popped_meta == "\"tag\"");

    CHECK(um.can_redo());
    CHECK(um.redo());
    CHECK(doc.get_text("t").to_string() == "hello");
}

// JSONPath: query a plain value and a nested container.
static void test_jsonpath() {
    loro::Doc doc;
    loro::Map users = doc.get_map("users");
    users.insert("alice", "30");
    // A nested map container under "bob".
    loro::Container bob = users.insert_container("bob", loro::Container::map());
    bob.as_map().insert("age", "25");
    doc.commit();

    loro::JsonPathResults r = doc.jsonpath("$.users.alice");
    CHECK(r.size() == 1);
    CHECK(!r.is_container(0));
    CHECK(r.value_json(0) == "30");

    loro::JsonPathResults rc = doc.jsonpath("$.users.bob");
    CHECK(rc.size() == 1);
    CHECK(rc.is_container(0));
    auto c = rc.container(0);
    CHECK(c.has_value());
    CHECK(c->as_map().get("age").value_or("") == "25");
}

// commit hooks + travel_change_ancestors: a message set before commit is read back from
// the change history.
static void test_commit_hooks_and_travel() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");

    // first-commit-from-peer fires once with this doc's peer id.
    int first_calls = 0;
    std::uint64_t seen_peer = 0;
    auto fsub = doc.subscribe_first_commit_from_peer([&](std::uint64_t peer) {
        ++first_calls;
        seen_peer = peer;
        return true;
    });

    // pre-commit rewrites the commit message.
    int pre_calls = 0;
    auto psub = doc.subscribe_pre_commit([&](const loro::PreCommitPayload& p) {
        ++pre_calls;
        p.set_message("hooked");
        return true;
    });

    t.insert(0, "x");
    doc.commit();
    CHECK(pre_calls >= 1);
    CHECK(first_calls == 1);
    CHECK(seen_peer == doc.peer_id());

    // Read the message back by traveling the change ancestors from the latest id.
    std::vector<loro::Id> tips = doc.oplog_frontiers().to_vec();
    CHECK(!tips.empty());
    std::vector<std::string> messages;
    doc.travel_change_ancestors(tips, [&](const loro::ChangeMeta& m) {
        messages.push_back(m.message());
        return true;  // visit all
    });
    bool found = false;
    for (const auto& m : messages)
        if (m == "hooked") found = true;
    CHECK(found);
}

// set_next_commit_message is the simpler, explicit path to the same effect.
static void test_next_commit_message() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    doc.set_next_commit_message("explicit-msg");
    t.insert(0, "y");
    doc.commit();

    std::vector<loro::Id> tips = doc.oplog_frontiers().to_vec();
    std::string msg;
    doc.travel_change_ancestors(tips, [&](const loro::ChangeMeta& m) {
        msg = m.message();
        return false;  // only the first (latest) change
    });
    CHECK(msg == "explicit-msg");
}

int main() {
    test_version_vector_sync();
    test_frontiers();
    test_time_travel();
    test_fractional_index();
    test_awareness();
    test_ephemeral_store();
    test_undo_manager();
    test_jsonpath();
    test_commit_hooks_and_travel();
    test_next_commit_message();

    if (failures == 0) {
        std::puts("test_advanced: OK");
        return 0;
    }
    std::fprintf(stderr, "test_advanced: %d failure(s)\n", failures);
    return 1;
}
