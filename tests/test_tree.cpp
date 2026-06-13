// M2: LoroTree — node creation/hierarchy, per-node metadata maps, moves, deletion, the
// fractional index for positional moves, and a snapshot round trip.

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

static bool same(loro::TreeId a, loro::TreeId b) {
    return a.peer == b.peer && a.counter == b.counter;
}

static void test_basic_hierarchy() {
    loro::Doc doc;
    loro::Tree tree = doc.get_tree("t");
    CHECK(tree.empty());

    loro::TreeId root = tree.create();        // a root node
    loro::TreeId child = tree.create(root);   // a child of root
    doc.commit();

    CHECK(!tree.empty());
    CHECK(tree.contains(root));
    CHECK(tree.contains(child));

    std::vector<loro::TreeId> roots = tree.roots();
    CHECK(roots.size() == 1);
    CHECK(same(roots[0], root));

    std::optional<std::vector<loro::TreeId>> kids = tree.children(root);
    CHECK(kids.has_value());
    CHECK(kids->size() == 1);
    CHECK(same((*kids)[0], child));
}

static void test_meta() {
    loro::Doc doc;
    loro::Tree tree = doc.get_tree("t");
    loro::TreeId node = tree.create();

    loro::Map meta = tree.get_meta(node);
    meta.insert("title", "\"hello\"");
    doc.commit();

    CHECK(tree.get_meta(node).get("title") == "\"hello\"");
}

static void test_move_and_delete() {
    loro::Doc doc;
    loro::Tree tree = doc.get_tree("t");
    loro::TreeId a = tree.create();
    loro::TreeId b = tree.create();  // two roots
    doc.commit();
    CHECK(tree.roots().size() == 2);

    tree.move_to(b, a);  // b becomes a child of a
    doc.commit();
    CHECK(tree.roots().size() == 1);
    std::optional<std::vector<loro::TreeId>> kids = tree.children(a);
    CHECK(kids.has_value());
    CHECK(kids->size() == 1);
    CHECK(same((*kids)[0], b));

    CHECK(!tree.is_node_deleted(b));
    tree.erase(b);
    doc.commit();
    CHECK(tree.is_node_deleted(b));
    // `contains` reports whether a node ever existed, so it stays true after deletion; the
    // node is gone from the live hierarchy, so a's children are now empty.
    std::optional<std::vector<loro::TreeId>> after = tree.children(a);
    CHECK(after.has_value());
    CHECK(after->empty());
}

static void test_fractional_index() {
    loro::Doc doc;
    loro::Tree tree = doc.get_tree("fi");
    // loro enables the fractional index by default (jitter 0); enabling again is a no-op.
    CHECK(tree.is_fractional_index_enabled());
    tree.enable_fractional_index(0);
    CHECK(tree.is_fractional_index_enabled());

    loro::TreeId a = tree.create();          // root index 0
    loro::TreeId b = tree.create_at(1);      // root index 1
    doc.commit();
    CHECK(tree.fractional_index(a).has_value());

    std::vector<loro::TreeId> roots = tree.roots();
    CHECK(roots.size() == 2);
    CHECK(same(roots[0], a));
    CHECK(same(roots[1], b));

    tree.move_to_index(b, 0);  // move b to the front
    doc.commit();
    roots = tree.roots();
    CHECK(roots.size() == 2);
    CHECK(same(roots[0], b));
    CHECK(same(roots[1], a));
}

static void test_snapshot_round_trip() {
    std::vector<std::uint8_t> snapshot;
    {
        loro::Doc doc;
        loro::Tree tree = doc.get_tree("t");
        loro::TreeId r = tree.create();
        tree.get_meta(r).insert("k", "\"v\"");
        doc.commit();
        snapshot = doc.export_snapshot();
    }
    CHECK(!snapshot.empty());

    loro::Doc fresh;
    fresh.import(snapshot);
    loro::Tree tree = fresh.get_tree("t");
    std::vector<loro::TreeId> roots = tree.roots();
    CHECK(roots.size() == 1);
    CHECK(tree.get_meta(roots[0]).get("k") == "\"v\"");
}

int main() {
    test_basic_hierarchy();
    test_meta();
    test_move_and_delete();
    test_fractional_index();
    test_snapshot_round_trip();

    if (failures == 0) {
        std::puts("test_tree: OK");
        return 0;
    }
    std::fprintf(stderr, "test_tree: %d failure(s)\n", failures);
    return 1;
}
