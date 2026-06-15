// json_sync — G3 sync surface. The same converge-without-a-server property as sync_two_docs,
// but exchanged over the *JSON* update format (the human-readable, peer-portable encoding you
// use to interop with JS / other-language peers that don't speak the binary format). Then it
// shows a shallow snapshot: a fresh peer can be brought up to the current state while dropping
// the deep history.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/json_sync

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    // Two peers, each with a distinct peer id so their ops never alias.
    loro::Doc alice;
    alice.set_peer_id(1);
    loro::Doc bob;
    bob.set_peer_id(2);

    // Alice writes and ships her history as JSON; Bob imports it (no binary blob involved).
    alice.get_text("doc").insert(0, "shared");
    alice.commit();
    const std::string base_json =
        alice.export_json_updates(loro::VersionVector::create(), alice.oplog_vv());
    std::printf("alice -> json (%zu bytes):\n%s\n\n", base_json.size(), base_json.c_str());
    bob.import_json_updates(base_json);

    // Concurrent edits on each side.
    alice.get_text("doc").insert(6, " by-alice");
    alice.commit();
    bob.get_text("doc").insert(0, "bob: ");
    bob.commit();

    // Exchange only the deltas, as JSON updates, in both directions.
    alice.import_json_updates(bob.export_json_updates(alice.oplog_vv(), bob.oplog_vv()));
    bob.import_json_updates(alice.export_json_updates(bob.oplog_vv(), alice.oplog_vv()));

    const std::string a = alice.get_text("doc").to_string();
    const std::string b = bob.get_text("doc").to_string();
    if (a != b) {
        std::fprintf(stderr, "divergence: peers did not converge\n");
        return 1;
    }
    std::printf("converged over JSON: %s\n\n", a.c_str());

    // Shallow snapshot: onboard a brand-new peer to the current state while trimming history.
    loro::Frontiers now = alice.state_frontiers();
    loro::Doc carol;
    carol.import(alice.export_shallow_snapshot(now));
    std::printf("carol (shallow): text=%s, is_shallow=%s, since=%s\n",
                carol.get_text("doc").to_string().c_str(),
                carol.is_shallow() ? "true" : "false",
                carol.shallow_since_vv().to_json().c_str());
    return 0;
}
