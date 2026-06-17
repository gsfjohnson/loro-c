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
    auto inner_it = deep_map->value.find("inner");
    if (inner_it == deep_map->value.end()) return fail("deep value missing 'inner'");

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
