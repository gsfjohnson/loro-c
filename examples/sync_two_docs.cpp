// sync_two_docs — two docs converging via incremental update exchange.
//
// Demonstrates: distinct peer IDs, edits on each side, export_updates_from /
// import on the peer, then verifying both docs settled to the same state.

#include <loro.hpp>
#include <loro/loro_ext.hpp>

#include <iostream>

namespace ext = loro::ext;

int main() {
    auto a = loro::LoroDoc::init();
    auto b = loro::LoroDoc::init();
    a->set_peer_id(1);
    b->set_peer_id(2);

    auto a_text = a->get_text(ext::root("body"));
    auto b_text = b->get_text(ext::root("body"));

    a_text->insert(0, "hello ");
    b_text->insert(0, "world");

    // Exchange updates from each side's pre-sync state vector.
    auto a_to_b = a->export_updates(b->state_vv());
    auto b_to_a = b->export_updates(a->state_vv());

    a->import(b_to_a);
    b->import(a_to_b);

    auto a_final = a_text->to_string();
    auto b_final = b_text->to_string();
    std::cout << "a: " << a_final << "\n";
    std::cout << "b: " << b_final << "\n";

    if (a_final != b_final) {
        std::cerr << "did not converge\n";
        return 1;
    }
    std::cout << "converged: " << a_final << "\n";
    return 0;
}
