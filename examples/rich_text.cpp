// rich_text — the G1 rich-text flow: format a range of text with marks (bold + a link),
// configure mark expand semantics, and inspect the result through the delta view.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/rich_text

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace ext = loro::ext;

// True if any insert/retain span in the delta carries the given style key.
static bool delta_has_attribute(const std::vector<loro::TextDelta> &delta,
                                const std::string &key) {
    for (const auto &d : delta) {
        const auto &v = d.get_variant();
        if (auto *ins = std::get_if<loro::TextDelta::kInsert>(&v)) {
            if (ins->attributes && ins->attributes->count(key)) return true;
        } else if (auto *ret = std::get_if<loro::TextDelta::kRetain>(&v)) {
            if (ret->attributes && ret->attributes->count(key)) return true;
        }
    }
    return false;
}

int main() {
    auto doc = loro::LoroDoc::init();

    // Configure how each style key expands when text is typed at its boundary: a "bold" run
    // grows as you type after it; a "link" run does not.
    auto styles = loro::StyleConfigMap::init();
    styles->insert("bold", loro::StyleConfig{loro::ExpandType::kAfter});
    styles->insert("link", loro::StyleConfig{loro::ExpandType::kNone});
    doc->config_text_style(styles);

    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "Hello world!");

    // Bold "Hello", and turn "world" into a link.
    text->mark(0, 5, "bold", ext::value_like_from(true));
    text->mark(6, 11, "link", ext::value_like_from(std::string("https://loro.dev")));
    doc->commit();

    std::cout << "text:  " << text->to_string() << "\n";

    // Typing right after the bold range inherits bold (After-expand); the link does not grow
    // when typing after it (None-expand).
    text->insert(5, "!!!");
    doc->commit();

    const auto delta = text->to_delta();
    const bool bold_present = delta_has_attribute(delta, "bold");
    std::cout << "bold present after edit: " << (bold_present ? "yes" : "no") << "\n";

    return bold_present ? 0 : 1;
}
