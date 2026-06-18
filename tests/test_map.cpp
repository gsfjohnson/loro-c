// Exercises LoroMap: insert / get / delete / keys / values / nested
// container creation via insert_*_container.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto map = doc->get_map(root("settings"));

    map->insert("name", str_value("loro"));
    map->insert("version", i64_value(1));
    map->insert("enabled", bool_value(true));

    if (map->len() != 3) return fail("map should have 3 entries");
    if (map->is_empty()) return fail("map should not be empty");

    auto name_voc = map->get("name");
    auto name_val = name_voc->as_value();
    if (!name_val.has_value() || loro_value_as_string(*name_val) != "loro") {
        return fail("map.get('name') wrong");
    }
    // A plain value reports is_value() and has no container_type (UPVERT_3).
    if (!name_voc->is_value()) return fail("plain-value voc should report is_value()");
    if (name_voc->container_type().has_value()) {
        return fail("plain-value voc should have no container_type()");
    }

    auto version_voc = map->get("version");
    auto version_val = version_voc->as_value();
    if (!version_val.has_value() || loro_value_as_i64(*version_val) != 1) {
        return fail("map.get('version') wrong");
    }

    map->delete_("enabled");
    if (map->len() != 2) return fail("map.len() != 2 after delete");

    auto keys = map->keys();
    if (keys.size() != 2) return fail("keys().size() != 2");

    auto values = map->values();
    if (values.size() != 2) return fail("values().size() != 2");

    auto detached_text = loro::LoroText::init();
    auto attached_text = map->insert_text_container("body", detached_text);
    attached_text->insert(0, "hello");

    auto body_voc = map->get("body");
    if (!body_voc->is_container()) return fail("map['body'] should be a container");
    // A live container reports !is_value() and a Text container_type (UPVERT_3).
    // Check before as_loro_text(), which takes the container handle out of the voc.
    if (body_voc->is_value()) return fail("container voc should not report is_value()");
    auto body_ty = body_voc->container_type();
    if (!body_ty.has_value() ||
        !std::holds_alternative<loro::ContainerType::kText>(body_ty->get_variant())) {
        return fail("map['body'] container_type() should be Text");
    }
    auto retrieved_text = body_voc->as_loro_text();
    if (retrieved_text->to_string() != "hello") {
        return fail("nested LoroText readback wrong");
    }

    auto detached_inner_map = loro::LoroMap::init();
    auto attached_inner = map->insert_map_container("inner", detached_inner_map);
    attached_inner->insert("k", str_value("v"));

    auto inner_voc = map->get("inner");
    auto inner_map = inner_voc->as_loro_map();
    auto inner_v = inner_map->get("k")->as_value();
    if (!inner_v.has_value() || loro_value_as_string(*inner_v) != "v") {
        return fail("nested LoroMap readback wrong");
    }

    auto deep = map->get_deep_value();
    auto *deep_map = std::get_if<loro::LoroValue::kMap>(&deep.get_variant());
    if (!deep_map) return fail("get_deep_value should be a map");
    auto inner_it = deep_map->value->find("inner");
    if (inner_it == deep_map->value->end()) return fail("deep value missing 'inner'");

    // Issue #1: get() on a missing key returns nullptr (loro-cpp parity), not a throw; and
    // contains() reports membership without throwing.
    if (map->get("does_not_exist") != nullptr) {
        return fail("map.get(absent) should return nullptr, not throw");
    }
    if (map->contains("does_not_exist")) return fail("contains(absent) should be false");
    if (!map->contains("name")) return fail("contains(present) should be true");

    // Issue #3: a detached container is editable *before* attach, and its buffered content
    // survives grafting via insert_*_container.
    auto detached_pre = loro::LoroMap::init();
    detached_pre->insert("kind", str_value("paragraph"));  // edited while detached
    detached_pre->insert("level", i64_value(2));
    map->insert_map_container("predetached", detached_pre);
    auto pre_map = map->get("predetached")->as_loro_map();
    auto kind_v = pre_map->get("kind")->as_value();
    if (!kind_v.has_value() || loro_value_as_string(*kind_v) != "paragraph") {
        return fail("detached-then-grafted map lost 'kind'");
    }
    auto level_v = pre_map->get("level")->as_value();
    if (!level_v.has_value() || loro_value_as_i64(*level_v) != 2) {
        return fail("detached-then-grafted map lost 'level'");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
