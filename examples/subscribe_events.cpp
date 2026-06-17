// subscribe_events — observe text edits via the lambda subscription helpers.
//
// Demonstrates: subscribe_root_lambda, subscribe_local_update_lambda,
// Subscription RAII, basic event counting.

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <atomic>
#include <iostream>

namespace ext = loro::ext;

int main() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(7);

    std::atomic<int> diff_events{0};
    std::atomic<int> local_updates{0};

    auto sub_root = ext::subscribe_root(*doc,
        [&](const loro::DiffEvent &) { diff_events.fetch_add(1); });

    auto sub_local = ext::subscribe_local_update(*doc,
        [&](const std::vector<uint8_t> &) { local_updates.fetch_add(1); });

    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "hi");
    doc->commit();
    text->insert(2, "!");
    doc->commit();

    std::cout << "diff events:   " << diff_events.load() << "\n";
    std::cout << "local updates: " << local_updates.load() << "\n";

    if (diff_events.load() == 0 || local_updates.load() == 0) {
        std::cerr << "expected at least one of each event\n";
        return 1;
    }
    return 0;
}
