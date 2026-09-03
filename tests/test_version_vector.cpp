// Exercises VersionVector and Frontiers: encode/decode round-trip,
// merge, includes_id, eq, get_last, partial_cmp, plus Frontiers from_id /
// from_ids / to_vec.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

// get_missing_span / diff read peers back from JSON. A peer above INT64_MAX used to make the
// parser throw std::out_of_range straight to the caller (gsfjohnson/loro-c#6).
bool large_peer_spans() {
    const uint64_t peers[] = {0x8000000000000000ULL, 0xfffffffffffffffeULL,
                              12012086296529505043ULL};
    for (uint64_t peer : peers) {
        auto doc = loro::LoroDoc::init();
        doc->set_peer_id(peer);
        auto text = doc->get_text(root("body"));
        text->insert(0, "abc");
        doc->commit();
        auto vv = doc->oplog_vv();
        auto empty_vv = loro::VersionVector::init();

        auto missing = empty_vv->get_missing_span(vv);
        if (missing.size() != 1) return fail("large peer: get_missing_span should have 1 span");
        if (missing[0].peer != peer) return fail("large peer: get_missing_span peer mismatch");
        if (missing[0].counter.start != 0 || missing[0].counter.end != 3) {
            return fail("large peer: get_missing_span counter span mismatch");
        }

        auto d = vv->diff(empty_vv);
        auto r = d.retreat.find(peer);
        if (r == d.retreat.end()) return fail("large peer: diff().retreat missing the peer");
        if (r->second.start != 0 || r->second.end != 3) {
            return fail("large peer: diff().retreat counter span mismatch");
        }
        if (!d.forward.empty()) return fail("large peer: diff().forward should be empty");

        auto d2 = empty_vv->diff(vv);
        if (d2.forward.find(peer) == d2.forward.end()) {
            return fail("large peer: diff().forward missing the peer");
        }
    }
    return true;
}

bool run() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(7);
    auto text = doc->get_text(root("body"));
    text->insert(0, "abc");
    doc->commit();

    auto vv = doc->state_vv();
    auto last = vv->get_last(7);
    if (!last.has_value() || *last <= 0) {
        return fail("state_vv missing entry for peer 7");
    }

    auto ovv = doc->oplog_vv();
    auto olast = ovv->get_last(7);
    if (!olast.has_value() || *olast <= 0) {
        return fail("oplog_vv missing entry for peer 7");
    }

    auto encoded = vv->encode();
    auto decoded = loro::VersionVector::decode(encoded);
    if (!decoded->eq(vv)) return fail("VersionVector encode/decode round-trip failed");

    auto empty_vv = loro::VersionVector::init();
    auto cmp = empty_vv->partial_cmp(vv);
    if (!cmp.has_value() || *cmp != loro::Ordering::kLess) {
        return fail("empty VV should compare less than vv");
    }
    if (!vv->includes_vv(empty_vv)) return fail("vv should include empty VV");

    auto missing = empty_vv->get_missing_span(vv);
    if (missing.empty()) return fail("get_missing_span should be non-empty");

    auto vv_copy = loro::VersionVector::init();
    vv_copy->merge(vv);
    if (!vv_copy->eq(vv)) return fail("merged copy should equal vv");

    auto frontiers = doc->state_frontiers();
    auto frontiers_encoded = frontiers->encode();
    auto frontiers_decoded = loro::Frontiers::decode(frontiers_encoded);
    if (!frontiers_decoded->eq(frontiers)) {
        return fail("Frontiers encode/decode round-trip failed");
    }
    if (frontiers->is_empty()) return fail("frontiers should be non-empty");

    auto ids = frontiers->to_vec();
    if (ids.empty()) return fail("frontiers.to_vec() empty");
    auto built = loro::Frontiers::from_ids(ids);
    if (!built->eq(frontiers)) return fail("Frontiers::from_ids round-trip failed");

    auto from_id = loro::Frontiers::from_id(ids.front());
    if (from_id->is_empty()) return fail("from_id frontiers should be non-empty");

    auto empty_f = loro::Frontiers::init();
    if (!empty_f->is_empty()) return fail("init() Frontiers should be empty");

    return large_peer_spans();
}

} // namespace

LORO_TEST_MAIN(run)
