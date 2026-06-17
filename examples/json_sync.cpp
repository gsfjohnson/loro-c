// json_sync — G3 sync surface. The same converge-without-a-server property as sync_two_docs,
// but exchanged over the *JSON* update format: the human-readable, peer-portable encoding you
// use to interop with JS / other-language peers that don't speak the binary format.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/json_sync

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <iostream>
#include <string>

namespace ext = loro::ext;

int main() {
    // Two peers, each with a distinct peer id so their ops never alias.
    auto alice = loro::LoroDoc::init();
    alice->set_peer_id(1);
    auto bob = loro::LoroDoc::init();
    bob->set_peer_id(2);

    auto alice_text = alice->get_text(ext::root("doc"));
    auto bob_text = bob->get_text(ext::root("doc"));

    // Alice writes and ships her whole history as JSON; Bob imports it (no binary blob
    // involved). The range is (empty .. alice's current state), i.e. everything she has.
    alice_text->insert(0, "shared");
    alice->commit();
    const std::string base_json =
        alice->export_json_updates(loro::VersionVector::init(), alice->state_vv());
    std::cout << "alice -> json (" << base_json.size() << " bytes):\n"
              << base_json << "\n\n";
    bob->import_json_updates(base_json);

    // Concurrent edits on each side.
    alice_text->insert(6, " by-alice");
    alice->commit();
    bob_text->insert(0, "bob: ");
    bob->commit();

    // Exchange only the deltas, as JSON updates, in both directions: each side exports the
    // range (what the peer already has .. what I have).
    alice->import_json_updates(bob->export_json_updates(alice->state_vv(), bob->state_vv()));
    bob->import_json_updates(alice->export_json_updates(bob->state_vv(), alice->state_vv()));

    const std::string a = alice_text->to_string();
    const std::string b = bob_text->to_string();
    if (a != b) {
        std::cerr << "divergence: peers did not converge\n";
        return 1;
    }
    std::cout << "converged over JSON: " << a << "\n";
    return 0;
}
