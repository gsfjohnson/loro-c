// basic_text — the canonical M1 flow: edit a text container, export a snapshot,
// import it into a fresh document, and read the value back.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/basic_text

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    std::printf("loro %s\n", loro::version().c_str());

    // Author a document.
    loro::Doc doc;
    loro::Text text = doc.get_text("greeting");
    text.insert(0, "hello");
    text.insert(5, " world");
    doc.commit();

    std::printf("authored: %s\n", text.to_string().c_str());
    std::printf("json:     %s\n", doc.to_json().c_str());

    // Snapshot it (full history + state).
    std::vector<std::uint8_t> snapshot = doc.export_snapshot();
    std::printf("snapshot: %zu bytes\n", snapshot.size());

    // Restore into an independent document and read it back.
    loro::Doc restored;
    restored.import(snapshot);
    const std::string round_trip = restored.get_text("greeting").to_string();
    std::printf("restored: %s\n", round_trip.c_str());

    return round_trip == "hello world" ? 0 : 1;
}
