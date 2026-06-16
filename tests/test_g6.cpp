// G6.1: doc config & timestamps — Doc::config() hands back a live Configure handle
// (record_timestamp / change-merge interval). The headline assertion is that the handle SHARES
// state with the document: a setting changed through one Configure handle (or via the Doc
// shortcuts) is observable through a freshly fetched handle, because Configure is backed by
// atomics behind Arc — not a snapshot.

#include <loro/loro.hpp>

#include <cstdio>
#include <string>

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

// ---- G6.2: doc history & introspection ----

// Op/change counts grow with edits; the pending transaction empties on commit.
static void test_history_counts() {
    loro::Doc doc;
    CHECK(doc.len_ops() == 0);
    CHECK(doc.len_changes() == 0);

    loro::Text t = doc.get_text("t");
    t.insert(0, "hello");
    // The edit sits in the pending (uncommitted) transaction.
    CHECK(doc.pending_txn_len() > 0);

    doc.commit();
    CHECK(doc.pending_txn_len() == 0);
    CHECK(doc.len_ops() >= 1);
    CHECK(doc.len_changes() == 1);
}

// has_container / get_path_to_container / try_get_* over real and bogus container ids.
static void test_container_introspection() {
    loro::Doc doc;
    loro::Map root = doc.get_map("root");
    loro::Container child = root.insert_container("child", loro::Container::text());
    std::string root_cid = root.id();
    std::string child_cid = child.id();
    doc.commit();

    CHECK(doc.has_container(root_cid));
    CHECK(doc.has_container(child_cid));
    CHECK(doc.has_container("cid:99@99:Text") == false);

    // The nested container's path runs through the parent map under key "child".
    std::string child_path = doc.get_path_to_container(child_cid);
    CHECK(child_path.find("child") != std::string::npos);
    CHECK(child_path.find(root_cid) != std::string::npos);

    // A bogus container has no path: NOT_FOUND surfaces as a thrown Error.
    bool threw = false;
    try {
        doc.get_path_to_container("cid:99@99:Text");
    } catch (const loro::Error&) {
        threw = true;
    }
    CHECK(threw);

    // try_get returns a live handle for an existing container, nullopt otherwise.
    CHECK(doc.try_get_map(root_cid).has_value());
    CHECK(doc.try_get_text(child_cid).has_value());
    CHECK(!doc.try_get_text("cid:99@99:Text").has_value());
    // Writing through a try_get handle reaches the document.
    auto live = doc.try_get_text(child_cid);
    live->insert(0, "hi");
    doc.commit();
    CHECK(doc.try_get_text(child_cid)->to_string() == "hi");
}

// get_changed_containers_in lists the container touched by a change range.
static void test_changed_containers() {
    loro::Doc doc;
    doc.set_peer_id(5);
    loro::Text t = doc.get_text("t");
    std::string t_cid = t.id();
    t.insert(0, "hi");
    doc.commit();

    std::string changed = doc.get_changed_containers_in(loro::Id{5, 0}, 2);
    CHECK(changed.find(t_cid) != std::string::npos);
}

// cmp_with_frontiers ordering, minimize_frontiers, and fork_at reproducing an old state.
static void test_frontiers_and_fork() {
    loro::Doc doc;
    doc.set_peer_id(1);
    loro::Text t = doc.get_text("t");
    t.insert(0, "AB");
    doc.commit();
    loro::Frontiers f0 = doc.state_frontiers();
    std::string state_at_f0 = doc.to_json();

    t.insert(2, "CD");
    doc.commit();

    // The doc is now ahead of F0, and equal to its own current frontiers.
    CHECK(doc.cmp_with_frontiers(f0) == 1);
    CHECK(doc.cmp_with_frontiers(doc.state_frontiers()) == 0);

    // minimize_frontiers of a known-good version yields a handle.
    CHECK(doc.minimize_frontiers(doc.state_frontiers()).has_value());

    // fork_at(F0) reproduces the F0 state and drops the later edit.
    auto forked = doc.fork_at(f0);
    CHECK(forked.has_value());
    CHECK(forked->get_text("t").to_string() == "AB");
    CHECK(forked->to_json() == state_at_f0);
}

// get_change resolves a change by its first-op id; bogus ids yield nullopt.
static void test_get_change() {
    loro::Doc doc;
    doc.set_peer_id(7);
    loro::Text t = doc.get_text("t");
    t.insert(0, "hello");
    doc.commit();

    auto change = doc.get_change(loro::Id{7, 0});
    CHECK(change.has_value());
    CHECK(change->id().peer == 7);
    CHECK(change->id().counter == 0);
    CHECK(change->size() >= 1);

    CHECK(!doc.get_change(loro::Id{7, 99999}).has_value());
    CHECK(!doc.get_change(loro::Id{999, 0}).has_value());
}

// Hiding empty root containers drops them from the deep value; cache controls are callable.
static void test_cache_and_hide_controls() {
    loro::Doc doc;
    doc.get_map("m"); // empty root container
    CHECK(doc.to_json().find("\"m\"") != std::string::npos);
    doc.set_hide_empty_root_containers(true);
    CHECK(doc.to_json().find("\"m\"") == std::string::npos);

    doc.get_text("t").insert(0, "x");
    doc.commit();
    // These free/compact internal caches; they must succeed without error.
    doc.free_history_cache();
    doc.free_diff_calculator();
    doc.compact_change_store();
    (void)doc.has_history_cache();
}

// ---- G6.3: doc method tail & commit options ----

// commit_with applies the message and timestamp, observable through the change metadata.
static void test_commit_with_options() {
    loro::Doc doc;
    doc.set_peer_id(11);
    doc.get_text("t").insert(0, "x");
    doc.commit_with(loro::CommitOptions().message("hello-msg").timestamp(1234567));

    auto ch = doc.get_change(loro::Id{11, 0});
    CHECK(ch.has_value());
    CHECK(ch->message() == "hello-msg");
    CHECK(ch->timestamp() == 1234567);
}

// set_next_commit_options stages options for the next commit; clear discards them; origin is
// callable.
static void test_next_commit_options() {
    {
        loro::Doc doc;
        doc.set_peer_id(12);
        doc.set_next_commit_options(loro::CommitOptions().message("opt-msg").timestamp(42));
        doc.get_text("t").insert(0, "a");
        doc.commit();
        auto ch = doc.get_change(loro::Id{12, 0});
        CHECK(ch.has_value());
        CHECK(ch->message() == "opt-msg");
        CHECK(ch->timestamp() == 42);
    }
    {
        // A clear drops the staged message: the commit lands with an empty message.
        loro::Doc doc;
        doc.set_peer_id(13);
        doc.set_next_commit_options(loro::CommitOptions().message("dropped"));
        doc.clear_next_commit_options();
        doc.get_text("t").insert(0, "a");
        doc.commit();
        auto ch = doc.get_change(loro::Id{13, 0});
        CHECK(ch.has_value());
        CHECK(ch->message().empty());
    }
    {
        // set_next_commit_origin is callable; the commit still lands as one change.
        loro::Doc doc;
        doc.set_peer_id(14);
        doc.set_next_commit_origin("ui");
        doc.get_text("t").insert(0, "a");
        doc.commit();
        CHECK(doc.len_changes() == 1);
    }
}

// detach freezes the doc state; attach re-syncs it.
static void test_attach_detach() {
    loro::Doc doc;
    doc.get_text("t").insert(0, "x");
    doc.commit();
    CHECK(doc.is_detached() == false);
    doc.detach();
    CHECK(doc.is_detached() == true);
    doc.attach();
    CHECK(doc.is_detached() == false);
}

// get_container resolves any container by id (type-erased); deep_value_with_id carries ids.
static void test_get_container_and_deep_value() {
    loro::Doc doc;
    loro::Map m = doc.get_map("m");
    m.insert("k", "\"v\"");
    std::string m_cid = m.id();
    doc.commit();

    auto c = doc.get_container(m_cid);
    CHECK(c.has_value());
    CHECK(c->type() == LORO_CONTAINER_MAP);
    CHECK(!doc.get_container("cid:99@99:Map").has_value());

    std::string dv = doc.deep_value_with_id();
    CHECK(dv.find("\"m\"") != std::string::npos);
}

// find_id_spans_between reports new ops under "forward" (and under "retreat" in reverse).
static void test_find_id_spans_between() {
    loro::Doc doc;
    doc.set_peer_id(1);
    loro::Frontiers f0 = doc.state_frontiers();
    doc.get_text("t").insert(0, "AB");
    doc.commit();
    loro::Frontiers f1 = doc.state_frontiers();

    std::string fwd = doc.find_id_spans_between(f0, f1);
    CHECK(fwd.find("forward") != std::string::npos);
    CHECK(fwd.find("retreat") != std::string::npos);
    CHECK(fwd.find("\"1\"") != std::string::npos); // peer 1 key

    std::string rev = doc.find_id_spans_between(f1, f0);
    CHECK(rev.find("\"1\"") != std::string::npos);
}

// ---- G6.4: per-container uniform introspection & attribution ----

// Every root container reports itself attached, not deleted, and hands back its doc.
static void test_g64_attached() {
    loro::Doc doc;

    loro::Text t = doc.get_text("t");
    CHECK(t.is_attached());
    CHECK(!t.is_deleted());
    CHECK(t.doc().has_value());

    loro::Map m = doc.get_map("m");
    CHECK(m.is_attached());
    CHECK(m.doc().has_value());

    loro::List l = doc.get_list("l");
    CHECK(l.is_attached());
    CHECK(l.doc().has_value());

    loro::MovableList ml = doc.get_movable_list("ml");
    CHECK(ml.is_attached());
    CHECK(ml.doc().has_value());

    loro::Tree tr = doc.get_tree("tr");
    CHECK(tr.is_attached());
    CHECK(tr.doc().has_value());

    loro::Counter ct = doc.get_counter("ct");
    CHECK(ct.is_attached());
    CHECK(ct.doc().has_value());
}

// A detached container (built via a Container factory) is not attached, has no doc, and
// cannot be subscribed to.
static void test_g64_detached() {
    loro::Container c = loro::Container::text();
    loro::Text t = c.as_text();
    CHECK(!t.is_attached());
    CHECK(!t.doc().has_value());
    CHECK(!t.subscribe([](const loro::DiffEvent&) {}).has_value());
}

// A child container becomes deleted once its key is removed from the parent.
static void test_g64_deleted() {
    loro::Doc doc;
    loro::Map root = doc.get_map("root");
    loro::Map child = root.insert_container("k", loro::Container::map()).as_map();
    doc.commit();
    CHECK(child.is_attached());
    CHECK(!child.is_deleted());

    root.remove("k");
    doc.commit();
    CHECK(child.is_deleted());
}

// A per-container subscription fires on commit and stops once released.
static void test_g64_subscribe_fires() {
    loro::Doc doc;
    loro::Text t = doc.get_text("text");
    int count = 0;
    auto sub = t.subscribe([&](const loro::DiffEvent&) { ++count; });
    CHECK(sub.has_value());

    t.insert(0, "hello");
    doc.commit();
    CHECK(count >= 1);

    int after_first = count;
    sub.reset();  // releasing the subscription unsubscribes
    t.insert(0, "x");
    doc.commit();
    CHECK(count == after_first);
}

// Attribution getters return the editing/creating/moving peer; out-of-range returns nullopt.
static void test_g64_attribution() {
    // Map: last editor of a key.
    {
        loro::Doc doc;
        doc.set_peer_id(42);
        loro::Map m = doc.get_map("m");
        m.insert("k", "1");
        doc.commit();
        auto ed = m.get_last_editor("k");
        CHECK(ed.has_value() && *ed == 42u);
        CHECK(!m.get_last_editor("absent").has_value());
    }
    // Text: editor at a unicode position.
    {
        loro::Doc doc;
        doc.set_peer_id(7);
        loro::Text t = doc.get_text("t");
        t.insert(0, "abc");
        doc.commit();
        auto e = t.get_editor_at_unicode_pos(0);
        CHECK(e.has_value() && *e == 7u);
        CHECK(!t.get_editor_at_unicode_pos(100).has_value());
    }
    // MovableList: creator / last mover / last editor of an element.
    {
        loro::Doc doc;
        doc.set_peer_id(9);
        loro::MovableList ml = doc.get_movable_list("ml");
        ml.insert(0, "1");
        doc.commit();
        auto cr = ml.get_creator_at(0);
        auto mv = ml.get_last_mover_at(0);
        auto ed = ml.get_last_editor_at(0);
        CHECK(cr.has_value() && *cr == 9u);
        CHECK(mv.has_value() && *mv == 9u);
        CHECK(ed.has_value() && *ed == 9u);
        // (Out-of-range positional queries clamp in loro's movable list rather than
        // returning nullopt, so there is no out-of-range negative assertion here.)
    }
    // Tree: last move id of a node (creation counts as the initial move).
    {
        loro::Doc doc;
        doc.set_peer_id(5);
        loro::Tree tr = doc.get_tree("tr");
        loro::TreeId a = tr.create();
        doc.commit();
        auto mid = tr.get_last_move_id(a);
        CHECK(mid.has_value());
        CHECK(mid.has_value() && mid->peer == 5u);

        loro::TreeId bogus{999999u, 12345};
        CHECK(!tr.get_last_move_id(bogus).has_value());
    }
}

int main() {
    test_config_defaults();
    test_config_handle_shares_live_state();
    test_doc_shortcuts();
    test_history_counts();
    test_container_introspection();
    test_changed_containers();
    test_frontiers_and_fork();
    test_get_change();
    test_cache_and_hide_controls();
    test_commit_with_options();
    test_next_commit_options();
    test_attach_detach();
    test_get_container_and_deep_value();
    test_find_id_spans_between();
    test_g64_attached();
    test_g64_detached();
    test_g64_deleted();
    test_g64_subscribe_fires();
    test_g64_attribution();

    if (failures == 0) {
        std::puts("test_g6: OK");
        return 0;
    }
    std::fprintf(stderr, "test_g6: %d failure(s)\n", failures);
    return 1;
}
