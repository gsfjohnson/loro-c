// Phase-4-in-scope slice of loro-cpp's tests/test_ext.cpp.
//
// Copies the undo case — test_undo_lambdas() — near-verbatim, exercising the loro::ext
// lambda -> OnPush/OnPop adapters and the ext::set_on_push / set_on_pop shortcuts. The other
// slices live in test_ext_containers.cpp (containers), test_ext_subscriptions.cpp (subscribe),
// and test_value_fidelity.cpp (value helpers). With the undo surface landed, loro-cpp's full
// test_ext.cpp now has all of its pieces covered across these files.
//
// loro-c-authored (not adopted wholesale from loro-cpp); built only under the conformance
// spike since it includes the spike <loro.hpp>.
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

#include <atomic>
#include <optional>

using namespace loro_test;
namespace ext = loro::ext;

namespace {

bool test_undo_lambdas() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(3);
    auto undo = loro::UndoManager::init(doc);

    std::atomic<int> push_count{0};
    std::atomic<int> pop_count{0};
    ext::set_on_push(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                                std::optional<loro::DiffEvent>) {
        push_count.fetch_add(1);
        return loro::UndoItemMeta{ext::value_null(), {}};
    });
    ext::set_on_pop(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                               const loro::UndoItemMeta &) {
        pop_count.fetch_add(1);
    });

    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "alpha");
    doc->commit();
    undo->record_new_checkpoint();

    text->insert(text->len_unicode(), " beta");
    doc->commit();
    undo->record_new_checkpoint();

    if (push_count.load() == 0) return fail("on_push lambda didn't fire");
    if (!undo->undo()) return fail("undo() returned false");
    if (pop_count.load() == 0) return fail("on_pop lambda didn't fire");

    return true;
}

bool run() {
    if (!test_undo_lambdas()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
