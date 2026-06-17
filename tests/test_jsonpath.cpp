// Exercises LoroDoc::jsonpath, get_by_str_path, and get_by_path.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto users = doc->get_map(root("users"));
    users->insert("alice", i64_value(30));
    users->insert("bob", i64_value(25));

    auto items = doc->get_list(root("items"));
    items->push(str_value("first"));
    items->push(str_value("second"));

    auto results = doc->jsonpath("$.users.alice");
    if (results.size() != 1) return fail("jsonpath('$.users.alice') size != 1");
    auto alice_val = results.front()->as_value();
    if (!alice_val.has_value() || loro_value_as_i64(*alice_val) != 30) {
        return fail("jsonpath returned wrong value for alice");
    }

    auto by_str = doc->get_by_str_path("items/0");
    auto first_val = by_str->as_value();
    if (!first_val.has_value() || loro_value_as_string(*first_val) != "first") {
        return fail("get_by_str_path('items/0') wrong");
    }

    std::vector<loro::Index> path;
    path.push_back(loro::Index(loro::Index::kKey{"users"}));
    path.push_back(loro::Index(loro::Index::kKey{"bob"}));
    auto by_path = doc->get_by_path(path);
    auto bob_val = by_path->as_value();
    if (!bob_val.has_value() || loro_value_as_i64(*bob_val) != 25) {
        return fail("get_by_path users/bob wrong");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
