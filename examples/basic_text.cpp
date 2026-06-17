// basic_text — minimal LoroDoc + LoroText edit + snapshot round-trip,
// using the loro_ext.hpp ergonomics layer.
//
// Demonstrates: doc init, root-text access via ext::root(), text edits,
// export_snapshot, import into a fresh doc, readback.

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <iostream>

namespace ext = loro::ext;

int main() {
    auto doc = loro::LoroDoc::init();

    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "hello");
    text->insert(5, ", world");

    std::cout << "text: " << text->to_string() << "\n";
    std::cout << "len:  " << text->len_unicode() << "\n";

    auto snapshot = doc->export_snapshot();
    std::cout << "snapshot: " << snapshot.size() << " bytes\n";

    auto doc2 = loro::LoroDoc::init();
    doc2->import(snapshot);
    auto text2 = doc2->get_text(ext::root("body"));

    if (text2->to_string() != "hello, world") {
        std::cerr << "round-trip mismatch: " << text2->to_string() << "\n";
        return 1;
    }
    std::cout << "round-trip ok\n";
    return 0;
}
