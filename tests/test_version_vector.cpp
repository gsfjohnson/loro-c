// Exercises VersionVector and Frontiers: encode/decode round-trip,
// merge, includes_id, eq, get_last, partial_cmp, plus Frontiers from_id /
// from_ids / to_vec.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

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

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
