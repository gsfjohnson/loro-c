// rich_text — the G1 rich-text flow: format a range of text with marks (bold + a link),
// configure mark expand semantics, and inspect the result through the delta view.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/rich_text

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    std::printf("loro %s\n", loro::version().c_str());

    loro::Doc doc;

    // Configure how each style key expands when text is typed at its boundary: a "bold"
    // run grows as you type after it; a "link" run does not.
    loro::StyleConfigMap styles;
    styles.insert("bold", LORO_EXPAND_AFTER);
    styles.insert("link", LORO_EXPAND_NONE);
    doc.config_text_style(styles);

    loro::Text text = doc.get_text("body");
    text.insert(0, "Hello world!");

    // Bold "Hello", and turn "world" into a link.
    text.mark(0, 5, "bold", "true");
    text.mark(6, 11, "link", "\"https://loro.dev\"");
    doc.commit();

    std::printf("text:  %s\n", text.to_string().c_str());
    std::printf("delta: %s\n", text.to_delta().c_str());

    // Typing right after the bold range inherits bold (After-expand); the link does not
    // grow when typing after it (None-expand).
    text.insert(5, "!!!");
    doc.commit();
    std::printf("after: %s\n", text.to_delta().c_str());

    // The bold attribute must still be present after the edit.
    const std::string delta = text.to_delta();
    return delta.find("bold") != std::string::npos ? 0 : 1;
}
