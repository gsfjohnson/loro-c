// Exercises the M3 ergonomics layer (include/loro/loro_ext.hpp):
//   * lambda → callback adapters (on_diff, on_local_update, on_pre_commit, ...)
//   * subscribe_*_lambda shortcuts on LoroDoc and per-container
//   * LoroValue construction (value_from / value_like) and inspection
//     (value_as_* / value_to_string)
//   * ContainerId helpers (root_text, root_map, root) and ContainerIdLike
//   * Templated container insertion (insert_container<LoroText>(map, key))
//   * Result<T> / try_call() success and failure paths
//   * UndoManager set_on_pop / set_on_push lambdas
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

#include <atomic>
#include <string>

using namespace loro_test;
namespace ext = loro::ext;

namespace {

bool test_value_helpers() {
    using loro::LoroValue;

    auto v_null = ext::value_null();
    auto v_bool = ext::value_from(true);
    auto v_i64  = ext::value_from(int64_t{42});
    auto v_dbl  = ext::value_from(3.5);
    auto v_str  = ext::value_from(std::string("hi"));
    auto v_cstr = ext::value_from("there");

    if (!ext::value_is_null(v_null)) return fail("value_null is not null");
    if (ext::value_as_bool(v_bool).value_or(false) != true)
        return fail("value_as_bool");
    if (ext::value_as_i64(v_i64).value_or(0) != 42) return fail("value_as_i64");
    if (ext::value_as_double(v_dbl).value_or(0.0) != 3.5)
        return fail("value_as_double");
    if (ext::value_as_string(v_str).value_or("") != "hi")
        return fail("value_as_string");
    if (ext::value_as_string(v_cstr).value_or("") != "there")
        return fail("value_as_string from cstr");

    if (ext::value_to_string(v_null) != "null") return fail("to_string null");
    if (ext::value_to_string(v_bool) != "true") return fail("to_string bool");
    if (ext::value_to_string(v_i64) != "42") return fail("to_string i64");
    if (ext::value_to_string(v_str) != "\"hi\"") return fail("to_string string");

    auto v_list = ext::value_from(std::vector<LoroValue>{
        ext::value_from(int64_t{1}),
        ext::value_from(std::string("two")),
    });
    if (ext::value_to_string(v_list) != "[1, \"two\"]")
        return fail("to_string list");

    return true;
}

bool test_container_id_helpers() {
    auto doc = loro::LoroDoc::init();

    // root() returns a polymorphic ContainerIdLike (kept type from request)
    auto text = doc->get_text(ext::root("body"));
    text->insert(0, "hello");
    auto cid = text->id();
    auto *root_id = std::get_if<loro::ContainerId::kRoot>(&cid.get_variant());
    if (!root_id || root_id->name != "body") {
        return fail("ext::root() didn't bind to expected container name");
    }

    // root_text(), root_map() etc. compose ContainerId directly.
    auto rid = ext::root_map("scratch");
    auto rmap = doc->get_map(ext::container_id_like(rid));
    rmap->insert("k", ext::value_like_from("v"));
    if (rmap->len() != 1) return fail("map insert via container_id_like");

    return true;
}

bool test_insert_container_template() {
    auto doc = loro::LoroDoc::init();
    auto m   = doc->get_map(ext::root("m"));

    auto child_text = ext::insert_container<loro::LoroText>(*m, "title");
    child_text->insert(0, "abc");
    if (child_text->len_unicode() != 3)
        return fail("insert_container<LoroText> didn't yield writable text");

    auto child_list = ext::insert_container<loro::LoroList>(*m, "items");
    child_list->push(ext::value_like_from(int64_t{1}));
    if (child_list->len() != 1)
        return fail("insert_container<LoroList> failed");

    auto pre = loro::LoroMap::init();
    auto child_map = ext::insert_container<loro::LoroMap>(*m, "nested",
                                                          std::move(pre));
    child_map->insert("k", ext::value_like_from("v"));
    if (child_map->len() != 1)
        return fail("insert_container<LoroMap>(child) failed");

    auto goc = ext::get_or_create_container<loro::LoroMap>(*m, "nested");
    if (goc->len() != 1)
        return fail("get_or_create_container should have returned same map");

    auto root_list = doc->get_list(ext::root("rl"));
    auto inserted_in_list =
        ext::insert_container<loro::LoroText>(*root_list, /*pos=*/0);
    inserted_in_list->insert(0, "xy");
    if (inserted_in_list->len_unicode() != 2)
        return fail("insert_container<LoroText> on list");

    auto mlist = doc->get_movable_list(ext::root("ml"));
    auto in_ml = ext::insert_container<loro::LoroCounter>(*mlist, /*pos=*/0);
    in_ml->increment(2.0);
    auto replaced = ext::set_container<loro::LoroText>(*mlist, /*pos=*/0);
    replaced->insert(0, "z");
    if (replaced->len_unicode() != 1)
        return fail("set_container on movable list");

    return true;
}

bool test_subscribe_lambdas() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(7);

    auto text = doc->get_text(ext::root("body"));
    auto cid  = text->id();

    std::atomic<int> root_count{0};
    std::atomic<int> cid_count{0};
    std::atomic<int> text_count{0};
    std::atomic<int> local_count{0};
    std::atomic<int> pre_count{0};
    std::atomic<int> first_count{0};

    auto sub_root = ext::subscribe_root(*doc, [&](const loro::DiffEvent &) {
        root_count.fetch_add(1);
    });
    auto sub_cid  = ext::subscribe(*doc, cid, [&](const loro::DiffEvent &) {
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

    if (root_count.load() == 0)  return fail("lambda subscribe_root didn't fire");
    if (cid_count.load() == 0)   return fail("lambda subscribe(doc,cid) didn't fire");
    if (text_count.load() == 0)  return fail("lambda subscribe(text) didn't fire");
    if (local_count.load() == 0) return fail("lambda subscribe_local_update didn't fire");
    if (pre_count.load() == 0)   return fail("lambda subscribe_pre_commit didn't fire");
    if (first_count.load() == 0) return fail("lambda subscribe_first_commit didn't fire");

    sub_root->unsubscribe();
    sub_cid->unsubscribe();
    sub_text->unsubscribe();
    sub_local->unsubscribe();
    sub_pre->unsubscribe();
    sub_first->unsubscribe();
    return true;
}

bool test_try_call() {
    auto doc = loro::LoroDoc::init();

    auto ok_void = ext::try_call([&] { doc->set_peer_id(99); });
    if (!ok_void) return fail("try_call(void) success path");
    if (!ok_void.error_what().empty()) return fail("ok void result has error_what");

    auto ok_t = ext::try_call([&] {
        auto text = doc->get_text(ext::root("body"));
        text->insert(0, "hi");
        return text->len_unicode();
    });
    if (!ok_t) return fail("try_call(T) success path");
    if (ok_t.value() != 2) return fail("try_call(T) returned wrong value");

    auto bad = ext::try_call([&] {
        std::vector<uint8_t> garbage{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        return doc->import(garbage);
    });
    if (bad) return fail("try_call should have failed on bad import");
    if (bad.error() == nullptr) return fail("failed Result missing exception_ptr");
    if (bad.error_what().empty()) return fail("failed Result missing what()");

    bool rethrew = false;
    try {
        bad.rethrow();
    } catch (const std::exception &) {
        rethrew = true;
    } catch (...) {
        rethrew = true;
    }
    if (!rethrew) return fail("rethrow() did not throw");

    auto bad_void = ext::try_call([&] {
        std::vector<uint8_t> garbage{0xff, 0xff, 0xff, 0xff};
        (void)doc->import(garbage);
    });
    if (bad_void) return fail("try_call(void) should have failed");
    if (bad_void.error_what().empty()) return fail("void failure missing what()");
    return true;
}

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

// Issue #2: cursors attached to an UndoItemMeta in on_push must round-trip to on_pop (the C ABI
// previously had no cursor channel, so they were silently dropped).
bool test_undo_cursor_roundtrip() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(9);
    auto undo = loro::UndoManager::init(doc);
    auto text = doc->get_text(ext::root("body"));

    std::atomic<bool> saw_cursor{false};
    std::atomic<int> pop_pos{-1};

    ext::set_on_push(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                                std::optional<loro::DiffEvent>) {
        loro::UndoItemMeta meta{ext::value_null(), {}};
        // Capture a cursor at the current end of the text (the position add_cursor records is
        // derived from the cursor itself, so the AbsolutePosition we pass here is a placeholder).
        auto cur = text->get_cursor(text->len_unicode(), loro::Side::kMiddle);
        if (cur) {
            meta.cursors.push_back(
                loro::CursorWithPos{cur, loro::AbsolutePosition{0, loro::Side::kMiddle}});
        }
        return meta;
    });
    ext::set_on_pop(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                               const loro::UndoItemMeta &m) {
        if (!m.cursors.empty() && m.cursors[0].cursor) {
            saw_cursor.store(true);
            pop_pos.store(static_cast<int>(m.cursors[0].pos.pos));
        }
    });

    text->insert(0, "hello");
    doc->commit();
    undo->record_new_checkpoint();
    text->insert(text->len_unicode(), " world");
    doc->commit();
    undo->record_new_checkpoint();

    if (!undo->undo()) return fail("undo() returned false");
    if (!saw_cursor.load()) {
        return fail("on_pop never received a cursor (issue #2 regression)");
    }
    if (pop_pos.load() < 0) return fail("popped cursor pos should be >= 0");

    return true;
}

bool run() {
    if (!test_value_helpers())          return false;
    if (!test_container_id_helpers())   return false;
    if (!test_insert_container_template()) return false;
    if (!test_subscribe_lambdas())      return false;
    if (!test_try_call())               return false;
    if (!test_undo_lambdas())           return false;
    if (!test_undo_cursor_roundtrip())  return false;
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
