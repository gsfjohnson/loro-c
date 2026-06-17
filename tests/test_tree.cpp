// Exercises LoroTree: create / mov / parent / children / nodes / get_meta /
// fractional index toggles.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool is_root(const loro::TreeParentId &p) {
    return std::holds_alternative<loro::TreeParentId::kRoot>(p.get_variant());
}

bool run() {
    auto doc = loro::LoroDoc::init();
    auto tree = doc->get_tree(root("forest"));

    auto root_id = tree->create(loro::TreeParentId(loro::TreeParentId::kRoot{}));
    auto child_a = tree->create(loro::TreeParentId(loro::TreeParentId::kNode{root_id}));
    auto child_b = tree->create(loro::TreeParentId(loro::TreeParentId::kNode{root_id}));

    if (!tree->contains(root_id)) return fail("tree should contain root_id");
    if (!tree->contains(child_a)) return fail("tree should contain child_a");

    auto roots = tree->roots();
    if (roots.size() != 1) return fail("roots() size != 1");

    auto kids = tree->children(loro::TreeParentId(loro::TreeParentId::kNode{root_id}));
    if (!kids.has_value() || kids->size() != 2) {
        return fail("root should have 2 children");
    }
    auto kids_count = tree->children_num(loro::TreeParentId(loro::TreeParentId::kNode{root_id}));
    if (!kids_count.has_value() || *kids_count != 2) {
        return fail("children_num != 2");
    }

    tree->mov(child_b, loro::TreeParentId(loro::TreeParentId::kNode{child_a}));
    auto new_parent = tree->parent(child_b);
    auto *as_node = std::get_if<loro::TreeParentId::kNode>(&new_parent.get_variant());
    if (!as_node || as_node->id.peer != child_a.peer || as_node->id.counter != child_a.counter) {
        return fail("mov did not relocate child_b under child_a");
    }

    auto root_parent = tree->parent(root_id);
    if (!is_root(root_parent)) return fail("root_id should have parent kRoot");

    auto meta = tree->get_meta(root_id);
    meta->insert("name", str_value("root-node"));
    auto v = meta->get("name")->as_value();
    if (!v.has_value() || loro_value_as_string(*v) != "root-node") {
        return fail("tree metadata roundtrip failed");
    }

    auto all_nodes = tree->nodes();
    if (all_nodes.size() != 3) return fail("nodes() size != 3");

    if (!tree->is_fractional_index_enabled()) {
        return fail("fractional index should be enabled by default");
    }
    auto frac = tree->fractional_index(root_id);
    if (!frac.has_value() || frac->empty()) {
        return fail("fractional_index for root_id missing");
    }

    tree->delete_(child_a);
    if (tree->is_node_deleted(child_a) == false) {
        return fail("child_a should report as deleted");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
