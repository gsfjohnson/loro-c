// G6.1: doc config & timestamps — Doc::config() hands back a live Configure handle
// (record_timestamp / change-merge interval). The headline assertion is that the handle SHARES
// state with the document: a setting changed through one Configure handle (or via the Doc
// shortcuts) is observable through a freshly fetched handle, because Configure is backed by
// atomics behind Arc — not a snapshot.

#include <loro/loro.hpp>

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

// A fresh doc reports the upstream defaults: timestamps off, merge interval 1000s.
static void test_config_defaults() {
    loro::Doc doc;
    loro::Configure cfg = doc.config();
    CHECK(cfg.record_timestamp() == false);
    CHECK(cfg.merge_interval() == 1000);
}

// Mutating through a Configure handle is visible through a *separately* fetched handle —
// proving the clone shares the document's live config rather than a copy.
static void test_config_handle_shares_live_state() {
    loro::Doc doc;
    doc.config().set_record_timestamp(true);
    doc.config().set_merge_interval(42);

    // A brand-new handle sees the changes.
    loro::Configure cfg = doc.config();
    CHECK(cfg.record_timestamp() == true);
    CHECK(cfg.merge_interval() == 42);

    // And a change through this handle is visible through yet another fresh one.
    cfg.set_record_timestamp(false);
    CHECK(doc.config().record_timestamp() == false);
}

// The Doc-level shortcuts agree with the Configure getters.
static void test_doc_shortcuts() {
    loro::Doc doc;

    doc.set_record_timestamp(true);
    CHECK(doc.config().record_timestamp() == true);
    doc.set_record_timestamp(false);
    CHECK(doc.config().record_timestamp() == false);

    doc.set_change_merge_interval(7);
    CHECK(doc.config().merge_interval() == 7);

    // Round-trips a large i64 (seconds) without truncation.
    doc.set_change_merge_interval(9000000000LL);
    CHECK(doc.config().merge_interval() == 9000000000LL);
}

int main() {
    test_config_defaults();
    test_config_handle_shares_live_state();
    test_doc_shortcuts();

    if (failures == 0) {
        std::puts("test_g6: OK");
        return 0;
    }
    std::fprintf(stderr, "test_g6: %d failure(s)\n", failures);
    return 1;
}
