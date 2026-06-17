// Phase-3-in-scope slice of loro-cpp's tests/test_ext.cpp.
//
// The full test_ext.cpp also exercises undo (Phase 4), so it cannot compile standalone against
// the Phase 3 header. This file copies the subscription case — test_subscribe_lambdas() —
// near-verbatim, exercising the loro::ext lambda → callback adapters and the subscribe_*
// shortcuts (the container cases are in test_ext_containers.cpp; the value-helper case is in
// test_value_fidelity.cpp). The full test_ext.cpp gets wired in once the undo surface lands.
//
// loro-c-authored (not adopted wholesale from loro-cpp); built only under the conformance
// spike since it includes the spike <loro.hpp>.
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

#include <atomic>
#include <cstdint>
#include <vector>

using namespace loro_test;
namespace ext = loro::ext;

namespace {

bool test_subscribe_lambdas() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(7);

    auto text = doc->get_text(ext::root("body"));
    auto cid = text->id();

    std::atomic<int> root_count{0};
    std::atomic<int> cid_count{0};
    std::atomic<int> text_count{0};
    std::atomic<int> local_count{0};
    std::atomic<int> pre_count{0};
    std::atomic<int> first_count{0};

    auto sub_root = ext::subscribe_root(*doc, [&](const loro::DiffEvent &) {
        root_count.fetch_add(1);
    });
    auto sub_cid = ext::subscribe(*doc, cid, [&](const loro::DiffEvent &) {
        cid_count.fetch_add(1);
    });
    auto sub_text = ext::subscribe(*text, [&](const loro::DiffEvent &) {
        text_count.fetch_add(1);
    });
    auto sub_local = ext::subscribe_local_update(
        *doc, [&](const std::vector<uint8_t> &) { local_count.fetch_add(1); });
    auto sub_pre = ext::subscribe_pre_commit(
        *doc, [&](const loro::PreCommitCallbackPayload &p) {
            pre_count.fetch_add(1);
            p.modifier->set_message("via-lambda");
        });
    auto sub_first = ext::subscribe_first_commit_from_peer(
        *doc, [&](const loro::FirstCommitFromPeerPayload &) {
            first_count.fetch_add(1);
        });

    text->insert(0, "ping");
    doc->commit();

    if (root_count.load() == 0) return fail("lambda subscribe_root didn't fire");
    if (cid_count.load() == 0) return fail("lambda subscribe(doc,cid) didn't fire");
    if (text_count.load() == 0) return fail("lambda subscribe(text) didn't fire");
    if (local_count.load() == 0) return fail("lambda subscribe_local_update didn't fire");
    if (pre_count.load() == 0) return fail("lambda subscribe_pre_commit didn't fire");
    if (first_count.load() == 0) return fail("lambda subscribe_first_commit didn't fire");

    sub_root->unsubscribe();
    sub_cid->unsubscribe();
    sub_text->unsubscribe();
    sub_local->unsubscribe();
    sub_pre->unsubscribe();
    sub_first->unsubscribe();
    return true;
}

bool run() {
    if (!test_subscribe_lambdas()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
