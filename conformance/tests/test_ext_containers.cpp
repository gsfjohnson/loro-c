// Phase-2-in-scope slice of loro-cpp's tests/test_ext.cpp.
//
// The full test_ext.cpp also exercises subscriptions (Phase 3) and undo (Phase 4), so it
// cannot compile standalone against the Phase 2 header. This file copies the two
// container-related cases — test_container_id_helpers() and test_insert_container_template()
// — near-verbatim (the value-helper case is already covered by test_value_fidelity.cpp). The
// full test_ext.cpp gets wired in once the subscription / undo surface lands.
//
// loro-c-authored (not adopted wholesale from loro-cpp); built only under the conformance
// spike since it includes the spike <loro.hpp>.
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

using namespace loro_test;
namespace ext = loro::ext;

namespace {

bool test_container_id_helpers() {
    auto doc = loro::LoroDoc::init();

    // root() returns a polymorphic ContainerIdLike (keeps the type from the request).
    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "hello");
    auto cid = text->id();
    auto *root_id = std::get_if<loro::ContainerId::kRoot>(&cid.get_variant());
    if (!root_id || root_id->name != "body") {
        return fail("ext::root() didn't bind to expected container name");
    }

    // root_text(), root_map() etc. compose ContainerId directly.
    auto rid = ext::root_map("scratch");
    auto rmap = doc->get_map(ext::container_id_like(rid));
    rmap->insert("k", ext::value_like_from("v"));
    if (rmap->len() != 1) return fail("map insert via container_id_like");

    return true;
}

bool test_insert_container_template() {
    auto doc = loro::LoroDoc::init();
    auto m = doc->get_map(ext::root("m"));

    auto child_text = ext::insert_container<loro::LoroText>(*m, "title");
    child_text->insert(0, "abc");
    if (child_text->len_unicode() != 3)
        return fail("insert_container<LoroText> didn't yield writable text");

    auto child_list = ext::insert_container<loro::LoroList>(*m, "items");
    child_list->push(ext::value_like_from(int64_t{1}));
    if (child_list->len() != 1) return fail("insert_container<LoroList> failed");

    auto pre = loro::LoroMap::init();
    auto child_map = ext::insert_container<loro::LoroMap>(*m, "nested", std::move(pre));
    child_map->insert("k", ext::value_like_from("v"));
    if (child_map->len() != 1) return fail("insert_container<LoroMap>(child) failed");

    auto goc = ext::get_or_create_container<loro::LoroMap>(*m, "nested");
    if (goc->len() != 1)
        return fail("get_or_create_container should have returned same map");

    auto root_list = doc->get_list(ext::root("rl"));
    auto inserted_in_list = ext::insert_container<loro::LoroText>(*root_list, /*pos=*/0);
    inserted_in_list->insert(0, "xy");
    if (inserted_in_list->len_unicode() != 2)
        return fail("insert_container<LoroText> on list");

    auto mlist = doc->get_movable_list(ext::root("ml"));
    auto in_ml = ext::insert_container<loro::LoroCounter>(*mlist, /*pos=*/0);
    in_ml->increment(2.0);
    auto replaced = ext::set_container<loro::LoroText>(*mlist, /*pos=*/0);
    replaced->insert(0, "z");
    if (replaced->len_unicode() != 1) return fail("set_container on movable list");

    return true;
}

bool run() {
    if (!test_container_id_helpers()) return false;
    if (!test_insert_container_template()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
