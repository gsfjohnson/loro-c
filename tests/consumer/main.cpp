// Install-tree smoke test: a tiny downstream program built against the installed package.
// Exercises a real snapshot round-trip so the static archive's symbols are actually linked
// (this is what validates the per-platform Libs.private / system-lib lists in the install).

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    loro::Doc doc;
    loro::Text text = doc.get_text("greeting");
    text.insert(0, "hello world");
    doc.commit();

    std::vector<std::uint8_t> snapshot = doc.export_snapshot();

    loro::Doc restored;
    restored.import(snapshot);
    const std::string round_trip = restored.get_text("greeting").to_string();

    std::printf("consumer: loro %s, round_trip=\"%s\"\n",
                loro::version().c_str(), round_trip.c_str());
    return round_trip == "hello world" ? 0 : 1;
}
