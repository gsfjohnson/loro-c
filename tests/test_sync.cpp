// G3: sync surface — JSON-update interchange (the human-readable format used to interop with
// JS / other-language peers), the export-mode family (shallow snapshot, snapshot-at, state-only,
// updates-in-range), import_batch / import_with, and shallow-history introspection.

#include <loro/loro.hpp>

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// export_json_updates from one doc -> import_json_updates into another reproduces the state.
static void test_json_round_trip() {
    loro::Doc a;
    a.set_peer_id(1);
    loro::Text ta = a.get_text("t");
    ta.insert(0, "hello world");
    a.commit();

    // Export everything: from an empty version vector up to the oplog version.
    std::string json = a.export_json_updates(loro::VersionVector::create(), a.oplog_vv());
    CHECK(!json.empty());
    CHECK(json.find("schema_version") != std::string::npos);
    CHECK(json.find("hello world") != std::string::npos);

    loro::Doc b;
    b.import_json_updates(json);
    CHECK(b.get_text("t").to_string() == "hello world");

    // The without-peer-compression variant is also valid JSON that round-trips.
    std::string json2 = a.export_json_updates_without_peer_compression(
        loro::VersionVector::create(), a.oplog_vv());
    CHECK(!json2.empty());
    CHECK(json2.find("hello world") != std::string::npos);
    loro::Doc c;
    c.import_json_updates(json2);
    CHECK(c.get_text("t").to_string() == "hello world");

    // export_json_in_id_span over peer 1's [0, 11) returns the change(s) for the insert.
    std::string span_json = a.export_json_in_id_span(1, 0, 11);
    CHECK(!span_json.empty());
    CHECK(span_json.find("hello world") != std::string::npos);
}

// Concurrent edits on two peers converge after exchanging JSON updates both directions.
static void test_json_concurrent_merge() {
    loro::Doc a;
    a.set_peer_id(1);
    a.get_text("t").insert(0, "Hello");
    a.commit();

    loro::Doc b;
    b.set_peer_id(2);
    b.import(a.export_snapshot());  // start b from a's state

    // Divergent edits.
    a.get_text("t").insert(5, " A");
    a.commit();
    b.get_text("t").insert(5, " B");
    b.commit();

    // Exchange the deltas as JSON updates both ways.
    a.import_json_updates(b.export_json_updates(a.oplog_vv(), b.oplog_vv()));
    b.import_json_updates(a.export_json_updates(b.oplog_vv(), a.oplog_vv()));

    CHECK(a.get_text("t").to_string() == b.get_text("t").to_string());
}

// The export-mode family: shallow snapshot, snapshot-at, state-only, updates-in-range.
static void test_export_modes() {
    loro::Doc doc;
    doc.set_peer_id(7);
    loro::Text t = doc.get_text("t");
    t.insert(0, "AAA");
    doc.commit();
    loro::Frontiers mid = doc.state_frontiers();  // version after the first commit
    t.insert(3, "BBB");
    doc.commit();

    // Shallow snapshot since `mid`: current state preserved, but the doc becomes shallow.
    {
        loro::Doc fresh;
        fresh.import(doc.export_shallow_snapshot(mid));
        CHECK(fresh.get_text("t").to_string() == "AAABBB");
        CHECK(fresh.is_shallow());
        CHECK(fresh.shallow_since_vv().to_json() != "{}");  // non-empty start version
    }
    CHECK(!doc.is_shallow());  // the source doc is unaffected

    // Snapshot at `mid` reconstructs the state as it was after the first commit.
    {
        loro::Doc fresh;
        fresh.import(doc.export_snapshot_at(mid));
        CHECK(fresh.get_text("t").to_string() == "AAA");
    }

    // State-only (latest) reconstructs the current state.
    {
        loro::Doc fresh;
        fresh.import(doc.export_state_only());  // nullptr -> latest
        CHECK(fresh.get_text("t").to_string() == "AAABBB");
    }

    // Updates in range over peer 7's whole span [0, 6) reconstruct the full content.
    {
        std::vector<LoroIdSpan> spans = {LoroIdSpan{7, 0, 6}};
        loro::Doc fresh;
        fresh.import(doc.export_updates_in_range(spans));
        CHECK(fresh.get_text("t").to_string() == "AAABBB");
    }
}

// import_batch merges several blobs at once; import_with attaches an origin and reproduces state.
static void test_import_batch_and_with() {
    loro::Doc p;
    p.set_peer_id(11);
    p.get_text("p").insert(0, "P");
    p.commit();
    std::vector<std::uint8_t> up = p.export_updates();

    loro::Doc q;
    q.set_peer_id(22);
    q.get_text("q").insert(0, "Q");
    q.commit();
    std::vector<std::uint8_t> uq = q.export_updates();

    loro::Doc r;
    r.import_batch({up, uq});
    CHECK(r.get_text("p").to_string() == "P");
    CHECK(r.get_text("q").to_string() == "Q");

    loro::Doc s;
    s.import_with(p.export_snapshot(), "telemetry-origin");
    CHECK(s.get_text("p").to_string() == "P");
}

int main() {
    test_json_round_trip();
    test_json_concurrent_merge();
    test_export_modes();
    test_import_batch_and_with();

    if (failures == 0) {
        std::puts("test_sync: OK");
        return 0;
    }
    std::fprintf(stderr, "test_sync: %d failure(s)\n", failures);
    return 1;
}
