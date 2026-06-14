// subscribe_events — the M3 flow: subscribe to a document, edit a container, and observe
// the DiffEvent that fires on commit (trigger kind, per-container target, JSON delta).
// Dropping the Subscription unsubscribes; no further events fire.
//
// Build (from the repo root, examples enabled):
//   cmake -S . -B build -DLORO_BUILD_EXAMPLES=ON
//   cmake --build build
//   ./build/examples/subscribe_events

#include <loro/loro.hpp>

#include <cstdio>

int main() {
    std::printf("loro %s\n", loro::version().c_str());

    loro::Doc doc;
    loro::Text text = doc.get_text("greeting");

    int events = 0;

    // subscribe_root fires for any committed change in the document. The DiffEvent (and the
    // ContainerDiff values obtained from it) are only valid for the duration of the callback.
    loro::Subscription sub = doc.subscribe_root([&](const loro::DiffEvent& ev) {
        ++events;
        const bool local = ev.triggered_by() == LORO_EVENT_TRIGGER_LOCAL;
        std::printf("event #%d: triggered_by=%s, %zu container diff(s)\n",
                    events, local ? "local" : "remote", ev.size());
        for (std::size_t i = 0; i < ev.size(); ++i) {
            loro::ContainerDiff d = ev[i];
            std::printf("  target=%s kind=%d delta=%s\n",
                        d.target().c_str(), static_cast<int>(d.kind()), d.to_json().c_str());
        }
    });

    // Each commit delivers one event describing the batched changes since the last commit.
    text.insert(0, "hello");
    doc.commit();

    text.insert(5, " world");
    doc.commit();

    {
        // Scoped subscription: it unsubscribes when `gone` is destroyed at the end of the block.
        int scoped = 0;
        loro::Subscription gone = doc.subscribe_root([&](const loro::DiffEvent&) { ++scoped; });
        text.insert(11, "!");
        doc.commit();
        // Both subscriptions saw this commit.
        std::printf("scoped subscription saw %d event(s)\n", scoped);
    }

    // `gone` is unsubscribed now; only the root subscription remains.
    const int before = events;
    text.remove(0, 6);  // drop "hello "
    doc.commit();
    std::printf("final text: %s\n", text.to_string().c_str());

    // The root subscription fired for every commit (4); the scoped one only for its single commit.
    return (events == 4 && events == before + 1) ? 0 : 1;
}
