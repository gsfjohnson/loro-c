// Exercises LoroText: insert / delete / mark / unmark / slice / splice /
// length variants / push_str / cursor stability across edits.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto text = doc->get_text(root("doc"));

    text->push_str("Hello");
    text->insert(text->len_unicode(), ", world!");
    if (text->to_string() != "Hello, world!") {
        return fail("push_str + insert produced wrong text");
    }
    if (text->len_unicode() != 13 || text->len_utf16() != 13 || text->len_utf8() != 13) {
        return fail("ASCII text length mismatch across encodings");
    }

    text->splice(7, 5, "Loro");
    if (text->to_string() != "Hello, Loro!") {
        return fail("splice did not replace expected range");
    }

    auto sliced = text->slice(0, 5);
    if (sliced != "Hello") return fail("slice(0, 5) wrong");

    auto cursor = text->get_cursor(5, loro::Side::kRight);
    if (!cursor) return fail("get_cursor returned null");
    text->insert(0, "[");
    auto pos_after = doc->get_cursor_pos(cursor);
    if (pos_after.current.pos != 6) {
        return fail("cursor did not shift right after prepending '['");
    }

    auto map = doc->get_map(root("style_cfg"));
    (void)map;
    auto cfg = loro::StyleConfigMap::default_rich_text_config();
    doc->config_text_style(cfg);

    text->mark(0, 5, "bold", bool_value(true));
    auto delta = text->to_delta();
    bool saw_bold = false;
    for (const auto &op : delta) {
        if (auto *ins = std::get_if<loro::TextDelta::kInsert>(&op.get_variant())) {
            if (ins->attributes.has_value() &&
                ins->attributes->find("bold") != ins->attributes->end()) {
                saw_bold = true;
            }
        }
    }
    if (!saw_bold) return fail("mark did not produce a bold attribute in delta");

    text->unmark(0, 5, "bold");
    auto delta_after_unmark = text->to_delta();
    bool still_bold = false;
    for (const auto &op : delta_after_unmark) {
        if (auto *ins = std::get_if<loro::TextDelta::kInsert>(&op.get_variant())) {
            if (ins->attributes.has_value()) {
                auto it = ins->attributes->find("bold");
                if (it != ins->attributes->end()) {
                    if (auto *b = std::get_if<loro::LoroValue::kBool>(&it->second.get_variant())) {
                        if (b->value) still_bold = true;
                    }
                }
            }
        }
    }
    if (still_bold) return fail("unmark did not clear bold attribute");

    text->delete_(0, 1);
    if (text->to_string() != "Hello, Loro!") {
        return fail("delete did not remove the bracket we prepended");
    }

    if (text->is_empty()) return fail("text should not be empty");
    if (!text->is_attached()) return fail("text from doc should be attached");

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
