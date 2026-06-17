// Exercises LoroCounter: increment / decrement / get_value across two
// peers + sync via export/import.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto counter = doc->get_counter(root("clicks"));

    if (counter->get_value() != 0.0) return fail("initial counter not 0");
    if (!counter->is_attached()) return fail("counter from doc should be attached");

    counter->increment(5.0);
    counter->increment(3.5);
    counter->decrement(1.0);
    if (counter->get_value() != 7.5) return fail("counter value != 7.5");

    auto doc2 = loro::LoroDoc::init();
    doc2->set_peer_id(2);
    auto counter2 = doc2->get_counter(root("clicks"));
    counter2->increment(2.5);

    doc2->import(doc->export_snapshot());
    doc->import(doc2->export_snapshot());

    if (counter->get_value() != counter2->get_value()) {
        return fail("counters did not converge after sync");
    }
    if (counter->get_value() != 10.0) {
        return fail("converged value != 10.0");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
