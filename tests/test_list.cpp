// Exercises LoroList: insert / push / pop / delete / get / len / to_vec /
// nested container creation via insert_*_container.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto list = doc->get_list(root("items"));

    list->push(i64_value(10));
    list->push(i64_value(20));
    list->push(i64_value(30));
    list->insert(1, i64_value(15));

    if (list->len() != 4) return fail("list.len() != 4");

    std::vector<int64_t> expected = {10, 15, 20, 30};
    for (uint32_t i = 0; i < 4; i++) {
        auto voc = list->get(i);
        auto v = voc->as_value();
        if (!v.has_value() || loro_value_as_i64(*v) != expected[i]) {
            return fail("list[i] wrong after insert");
        }
    }

    // Issue #1: get() with an out-of-range index returns nullptr (loro-cpp parity), not a throw.
    if (list->get(999) != nullptr) return fail("list.get(out-of-range) should return nullptr");

    auto popped = list->pop();
    if (!popped.has_value() || loro_value_as_i64(*popped) != 30) {
        return fail("pop should return 30");
    }
    if (list->len() != 3) return fail("len should be 3 after pop");

    list->delete_(0, 1);
    if (list->len() != 2) return fail("len should be 2 after delete(0, 1)");

    auto vec = list->to_vec();
    if (vec.size() != 2) return fail("to_vec size != 2");
    if (loro_value_as_i64(vec[0]) != 15 || loro_value_as_i64(vec[1]) != 20) {
        return fail("to_vec contents wrong");
    }

    auto detached_text = loro::LoroText::init();
    auto attached = list->insert_text_container(0, detached_text);
    attached->insert(0, "first");

    auto first_voc = list->get(0);
    if (!first_voc->is_container()) return fail("list[0] should be container");
    auto t = first_voc->as_loro_text();
    if (t->to_string() != "first") return fail("nested text readback wrong");

    // Issue #3: a detached list is editable before attach; its content survives grafting.
    auto detached_list = loro::LoroList::init();
    detached_list->push(i64_value(100));  // edited while detached
    detached_list->push(i64_value(200));
    list->insert_list_container(0, detached_list);
    auto nested = list->get(0)->as_loro_list();
    if (nested->len() != 2) return fail("detached-then-grafted list lost items");
    if (loro_value_as_i64(*nested->get(0)->as_value()) != 100) {
        return fail("detached-then-grafted list[0] wrong");
    }
    if (loro_value_as_i64(*nested->get(1)->as_value()) != 200) {
        return fail("detached-then-grafted list[1] wrong");
    }

    list->clear();
    if (list->len() != 0) return fail("len != 0 after clear");
    if (!list->is_empty()) return fail("should be empty after clear");

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
