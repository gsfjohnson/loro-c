// Exercises FractionalIndex: round-trip via from_hex_string / to_string and
// the tree's fractional_index() method.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto tree = doc->get_tree(root("forest"));
    auto a = tree->create(loro::TreeParentId(loro::TreeParentId::kRoot{}));
    auto b = tree->create(loro::TreeParentId(loro::TreeParentId::kRoot{}));

    auto frac_a = tree->fractional_index(a);
    auto frac_b = tree->fractional_index(b);
    if (!frac_a.has_value() || !frac_b.has_value()) {
        return fail("missing fractional_index for sibling tree nodes");
    }
    if (frac_a == frac_b) return fail("siblings should have distinct fractional indices");

    auto fi = loro::FractionalIndex::from_hex_string(*frac_a);
    if (fi->to_string() != *frac_a) {
        return fail("FractionalIndex hex round-trip mismatch");
    }

    auto bytes_round_trip = loro::FractionalIndex::from_bytes({0x80});
    if (bytes_round_trip->to_string().empty()) {
        return fail("FractionalIndex from_bytes produced empty string");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
