// Install-tree smoke test: a tiny downstream program built against the installed package.
// Exercises a real snapshot round-trip so the static archive's symbols are actually linked
// (this is what validates the per-platform Libs.private / system-lib lists in the install).
// It includes both installed headers — <loro.hpp> and <loro/loro_ext.hpp> — so a missing or
// mis-located header in the install tree fails here too.

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <cstdio>
#include <string>
#include <vector>

int main() {
    auto doc = loro::LoroDoc::init();
    auto text = doc->get_text(loro::ext::root("greeting"));
    text->insert(0, "hello world");
    doc->commit();

    std::vector<std::uint8_t> snapshot = doc->export_snapshot();

    auto restored = loro::LoroDoc::init();
    restored->import(snapshot);
    const std::string round_trip =
        restored->get_text(loro::ext::root("greeting"))->to_string();

    std::printf("consumer: round_trip=\"%s\"\n", round_trip.c_str());
    return round_trip == "hello world" ? 0 : 1;
}
