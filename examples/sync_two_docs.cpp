// sync_two_docs — two peers edit independently, exchange their operation logs, and
// converge to the same state. This is the CRDT property in miniature: concurrent edits
// from different peers merge without a central server and without conflicts.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/sync_two_docs

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    // Two peers, each with a distinct peer id so their ops never alias.
    loro::Doc alice;
    alice.set_peer_id(1);
    loro::Doc bob;
    bob.set_peer_id(2);

    // Start from a shared base: Alice writes, snapshots, Bob imports it.
    loro::Text alice_text = alice.get_text("doc");
    alice_text.insert(0, "shared");
    alice.commit();
    bob.import(alice.export_snapshot());

    // Now they edit concurrently, without seeing each other's change yet.
    alice_text.insert(6, " by-alice");  // -> "shared by-alice"
    alice.commit();

    loro::Text bob_text = bob.get_text("doc");
    bob_text.insert(0, "bob: ");        // -> "bob: shared"
    bob.commit();

    // Exchange operation logs in both directions and merge.
    const std::vector<std::uint8_t> from_alice = alice.export_updates();
    const std::vector<std::uint8_t> from_bob = bob.export_updates();
    alice.import(from_bob);
    bob.import(from_alice);

    const std::string a = alice.get_text("doc").to_string();
    const std::string b = bob.get_text("doc").to_string();
    std::printf("alice: %s\n", a.c_str());
    std::printf("bob:   %s\n", b.c_str());

    // CRDT guarantee: both peers converge to byte-identical state.
    if (a != b) {
        std::fprintf(stderr, "divergence: peers did not converge\n");
        return 1;
    }
    std::printf("converged: %s\n", a.c_str());
    return 0;
}
