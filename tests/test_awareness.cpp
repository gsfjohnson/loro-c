// Exercises Awareness: encode_all / apply / get_all_states / set_local_state.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto a1 = loro::Awareness::init(/*peer=*/1, /*timeout_ms=*/30'000);
    auto a2 = loro::Awareness::init(/*peer=*/2, /*timeout_ms=*/30'000);

    if (a1->peer() != 1) return fail("peer 1 wrong");
    if (a2->peer() != 2) return fail("peer 2 wrong");

    if (a1->get_local_state().has_value()) {
        return fail("a1 local state should start empty");
    }

    a1->set_local_state(str_value("alice-online"));
    auto local = a1->get_local_state();
    if (!local.has_value() || loro_value_as_string(*local) != "alice-online") {
        return fail("a1 local state didn't round-trip");
    }

    auto encoded = a1->encode_all();
    if (encoded.empty()) return fail("encode_all returned empty");

    auto update = a2->apply(encoded);
    bool saw_peer_1 = false;
    for (auto p : update.added) {
        if (p == 1) saw_peer_1 = true;
    }
    if (!saw_peer_1) return fail("apply did not surface peer 1 as added");

    auto states = a2->get_all_states();
    auto it = states.find(1);
    if (it == states.end()) return fail("a2 missing state for peer 1");
    if (loro_value_as_string(it->second.state) != "alice-online") {
        return fail("a2's view of peer 1 state wrong");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
