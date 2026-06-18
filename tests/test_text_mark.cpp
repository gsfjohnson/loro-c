// Exercises LoroText::mark with list/map-valued attributes — issue #2 latent gap.
//
// mark() marshals its value through detail::value_to_json. Pre-fix that threw "conformance
// spike: only scalar LoroValue kinds cross as JSON" for any list/map value. The fix recurses
// into list/map. We verify end-to-end via the public mark() + to_delta() round-trip (to_delta
// parses the attribute JSON back into a LoroValue), and assert on the parsed structure rather
// than raw JSON (double formatting and map key order are not stable).
#include "test_helpers.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

using namespace loro_test;

namespace {

std::shared_ptr<loro::LoroValueLike> value_like(loro::LoroValue v) {
    return std::make_shared<PrimitiveValue>(std::move(v));
}

loro::LoroValue lv_str(std::string s) {
    return loro::LoroValue(loro::LoroValue::kString{std::move(s)});
}
loro::LoroValue lv_i64(int64_t v) { return loro::LoroValue(loro::LoroValue::kI64{v}); }
loro::LoroValue lv_list(std::vector<loro::LoroValue> v) {
    return loro::LoroValue(
        loro::LoroValue::kList{std::make_shared<std::vector<loro::LoroValue>>(std::move(v))});
}
loro::LoroValue lv_map(std::unordered_map<std::string, loro::LoroValue> m) {
    return loro::LoroValue(loro::LoroValue::kMap{
        std::make_shared<std::unordered_map<std::string, loro::LoroValue>>(std::move(m))});
}

// Finds the value of mark attribute `key` across the insert deltas (the marked text segment
// carries the attributes).
std::optional<loro::LoroValue> find_attr(const std::vector<loro::TextDelta> &deltas,
                                         const std::string &key) {
    for (const auto &d : deltas) {
        if (auto *ins = std::get_if<loro::TextDelta::kInsert>(&d.get_variant())) {
            if (ins->attributes) {
                auto it = ins->attributes->find(key);
                if (it != ins->attributes->end()) return it->second;
            }
        }
    }
    return std::nullopt;
}

// Marks "hello"[0,5] with (key, value) on a fresh text container and returns the attribute as
// read back through to_delta().
std::optional<loro::LoroValue> mark_and_read(const std::shared_ptr<loro::LoroDoc> &doc,
                                             const std::string &root_name, const std::string &key,
                                             const std::shared_ptr<loro::LoroValueLike> &value) {
    auto text = doc->get_text(root(root_name));
    text->insert(0, "hello");
    text->mark(0, 5, key, value);
    return find_attr(text->to_delta(), key);
}

bool run() {
    auto doc = loro::LoroDoc::init();

    // loro requires every mark key to have a registered text-style config (expand behaviour);
    // an unconfigured key throws "Style configuration missing". Register the keys we mark with.
    auto styles = loro::StyleConfigMap::init();
    for (const char *k : {"bold", "ids", "meta", "block"}) {
        styles->insert(k, loro::StyleConfig{loro::ExpandType::kAfter});
    }
    doc->config_text_style(styles);

    // (1) Scalar regression — a bool attribute round-trips as kBool{true}.
    {
        auto attr = mark_and_read(doc, "t_bool", "bold", bool_value(true));
        if (!attr) return fail("bool mark attribute missing");
        auto *b = std::get_if<loro::LoroValue::kBool>(&attr->get_variant());
        if (!b || !b->value) return fail("bool mark attribute wrong");
    }

    // (2) List attribute — pre-fix this threw in value_to_json.
    {
        auto attr = mark_and_read(doc, "t_list", "ids",
                                  value_like(lv_list({lv_i64(1), lv_str("a")})));
        if (!attr) return fail("list mark attribute missing");
        auto *l = std::get_if<loro::LoroValue::kList>(&attr->get_variant());
        if (!l || l->value->size() != 2) return fail("list mark attribute not a 2-element list");
        // element 0: numeric 1 (accept kI64 or kDouble — JSON number type is not load-bearing).
        const loro::LoroValue &e0 = (*l->value)[0];
        bool e0_ok = false;
        if (auto *i = std::get_if<loro::LoroValue::kI64>(&e0.get_variant())) e0_ok = i->value == 1;
        else if (auto *d = std::get_if<loro::LoroValue::kDouble>(&e0.get_variant()))
            e0_ok = d->value == 1.0;
        if (!e0_ok) return fail("list mark attribute element 0 wrong");
        if (loro_value_as_string((*l->value)[1]) != "a") {
            return fail("list mark attribute element 1 wrong");
        }
    }

    // (3) Single-key map attribute.
    {
        auto attr = mark_and_read(doc, "t_map", "meta", value_like(lv_map({{"k", lv_str("v")}})));
        if (!attr) return fail("map mark attribute missing");
        auto *m = std::get_if<loro::LoroValue::kMap>(&attr->get_variant());
        if (!m || m->value->size() != 1) return fail("map mark attribute not a 1-entry map");
        auto it = m->value->find("k");
        if (it == m->value->end() || loro_value_as_string(it->second) != "v") {
            return fail("map mark attribute value wrong");
        }
    }

    // (4) Nested list-in-map — confirms the recursion.
    {
        auto attr = mark_and_read(doc, "t_nested", "block",
                                  value_like(lv_map({{"items", lv_list({lv_str("x"), lv_str("y")})}})));
        if (!attr) return fail("nested mark attribute missing");
        auto *m = std::get_if<loro::LoroValue::kMap>(&attr->get_variant());
        if (!m) return fail("nested mark attribute not a map");
        auto it = m->value->find("items");
        if (it == m->value->end()) return fail("nested map missing 'items'");
        auto *inner = std::get_if<loro::LoroValue::kList>(&it->second.get_variant());
        if (!inner || inner->value->size() != 2) return fail("nested list wrong size");
        if (loro_value_as_string((*inner->value)[0]) != "x" ||
            loro_value_as_string((*inner->value)[1]) != "y") {
            return fail("nested list contents wrong");
        }
    }

    // (5) Binary attribute is still rejected (JSON has no native binary form).
    {
        auto text = doc->get_text(root("t_bin"));
        text->insert(0, "hello");
        auto bin = value_like(loro::LoroValue(loro::LoroValue::kBinary{{1, 2, 3}}));
        bool threw = false;
        try {
            text->mark(0, 5, "raw", bin);
        } catch (const loro::LoroError &) {
            threw = true;
        }
        if (!threw) return fail("binary mark value should throw");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
