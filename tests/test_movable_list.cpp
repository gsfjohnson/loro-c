// Exercises LoroMovableList: insert / push / set / mov / pop / delete / get.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run() {
    auto doc = loro::LoroDoc::init();
    auto list = doc->get_movable_list(root("queue"));

    list->push(str_value("a"));
    list->push(str_value("b"));
    list->push(str_value("c"));
    if (list->len() != 3) return fail("len != 3 after pushes");

    list->set(1, str_value("B"));
    auto v_b = list->get(1)->as_value();
    if (!v_b.has_value() || loro_value_as_string(*v_b) != "B") {
        return fail("set(1, 'B') did not stick");
    }

    list->mov(0, 2);
    auto v0 = list->get(0)->as_value();
    auto v2 = list->get(2)->as_value();
    if (loro_value_as_string(*v0) != "B") return fail("after mov(0,2), pos 0 should be 'B'");
    if (loro_value_as_string(*v2) != "a") return fail("after mov(0,2), pos 2 should be 'a'");

    auto popped_voc = list->pop();
    auto popped_val = popped_voc->as_value();
    if (!popped_val.has_value() || loro_value_as_string(*popped_val) != "a") {
        return fail("pop should return 'a'");
    }
    if (list->len() != 2) return fail("len != 2 after pop");

    list->delete_(0, 1);
    if (list->len() != 1) return fail("len != 1 after delete(0,1)");

    auto detached = loro::LoroText::init();
    auto attached_text = list->insert_text_container(0, detached);
    attached_text->insert(0, "nested");
    auto t_voc = list->get(0);
    if (!t_voc->is_container()) return fail("inserted container not visible as container");
    if (t_voc->as_loro_text()->to_string() != "nested") {
        return fail("nested text content wrong");
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
