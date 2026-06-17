// Exercises LoroText's UTF-8 *byte-indexed* mutators: insert_utf8 / delete_utf8 / mark_utf8.
// These cover the seam where UTF-8 byte offsets and Unicode codepoint indices diverge for
// non-ASCII text. loro-c-authored: the adopted loro-cpp test_text.cpp uses only ASCII text and
// codepoint indexing, so it never exercises this divergence.
//
// Test fixture: "\xC3\xA1" is U+00E1 (LATIN SMALL LETTER A WITH ACUTE), 2 UTF-8 bytes; "b" is
// 1 byte. So the string is 3 UTF-8 bytes but only 2 Unicode codepoints. (\x is greedy in C++,
// so the multibyte char must be its own string-literal piece, kept apart from following hex.)
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto text = doc->get_text(root("doc"));

    text->insert_utf8(0, "\xC3\xA1" "b");
    if (text->to_string() != "\xC3\xA1" "b") {
        return fail("insert_utf8 produced wrong text");
    }
    if (text->len_utf8() != 3) return fail("len_utf8 should be 3 bytes");
    if (text->len_unicode() != 2) return fail("len_unicode should be 2 codepoints");
    if (text->len_utf8() == text->len_unicode()) {
        return fail("expected byte/codepoint length divergence for non-ASCII text");
    }

    // Byte offset 2 is right after the 2-byte 'a-acute' (which is codepoint index 1). A
    // byte-correct insert lands between the two characters; a codepoint-index insert at 2 would
    // instead have appended at the end. Asserting the former distinguishes correct byte
    // addressing from the naive insert()/insert_utf8() mix-up bug.
    text->insert_utf8(2, "-");
    if (text->to_string() != "\xC3\xA1" "-b") {
        return fail("insert_utf8 placed text at the wrong byte offset");
    }

    // Delete the single '-' byte we just inserted at byte offset 2.
    text->delete_utf8(2, 1);
    if (text->to_string() != "\xC3\xA1" "b") {
        return fail("delete_utf8 removed the wrong bytes");
    }

    // mark_utf8 over the byte range [0, 2) covers exactly the 'a-acute' codepoint.
    auto cfg = loro::StyleConfigMap::default_rich_text_config();
    doc->config_text_style(cfg);
    text->mark_utf8(0, 2, "bold", bool_value(true));

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
    if (!saw_bold) return fail("mark_utf8 did not produce a bold attribute in delta");

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
