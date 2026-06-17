// Fidelity tests for the typed-value bridge (RESHAPE Phase 1).
//
// These cover the conformance suite's blind spots — loro-cpp's own tests only check `3.5`
// (a fractional double) and never exercise binary, so the lossy JSON bridge would pass them
// while corrupting data. Here we assert:
//   * kDouble{2.0} — an integer-valued double — round-trips as kDouble, NOT kI64.
//   * kBinary round-trips as kBinary, NOT a kList.
//   * the loro::ext value helpers (the value-helper portion of loro-cpp's test_ext, which
//     cannot be compiled standalone in Phase 1 because the rest of that file needs the
//     subscription / undo / container-template surface from later phases).
//
// loro-c-authored (not adopted from loro-cpp); built only under the conformance spike since
// it includes the spike <loro.hpp>.
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

#include <cstdint>
#include <variant>
#include <vector>

using namespace loro_test;
namespace ext = loro::ext;

namespace {

// Mirrors test_ext.cpp's test_value_helpers() (the Phase-1-in-scope portion).
bool test_value_helpers() {
    using loro::LoroValue;

    if (!ext::value_is_null(ext::value_null())) return fail("value_null is not null");
    if (ext::value_as_bool(ext::value_from(true)).value_or(false) != true)
        return fail("value_as_bool");
    if (ext::value_as_i64(ext::value_from(int64_t{42})).value_or(0) != 42)
        return fail("value_as_i64");
    if (ext::value_as_double(ext::value_from(3.5)).value_or(0.0) != 3.5)
        return fail("value_as_double");
    if (ext::value_as_string(ext::value_from(std::string("hi"))).value_or("") != "hi")
        return fail("value_as_string");
    if (ext::value_as_string(ext::value_from("there")).value_or("") != "there")
        return fail("value_as_string from cstr");

    if (ext::value_to_string(ext::value_null()) != "null") return fail("to_string null");
    if (ext::value_to_string(ext::value_from(true)) != "true") return fail("to_string bool");
    if (ext::value_to_string(ext::value_from(int64_t{42})) != "42") return fail("to_string i64");
    if (ext::value_to_string(ext::value_from(std::string("hi"))) != "\"hi\"")
        return fail("to_string string");

    auto list = ext::value_from(std::vector<LoroValue>{
        ext::value_from(int64_t{1}),
        ext::value_from(std::string("two")),
    });
    if (ext::value_to_string(list) != "[1, \"two\"]") return fail("to_string list");

    return true;
}

// 2.0 is an integer-valued double; the JSON bridge would collapse it to I64.
bool test_double_fidelity() {
    using loro::LoroValue;
    auto doc = loro::LoroDoc::init();
    auto map = doc->get_map(root("m"));
    map->insert("d", ext::value_like_from(2.0));

    auto v = map->get("d")->as_value();
    if (!v.has_value()) return fail("double get returned no value");
    if (!std::holds_alternative<LoroValue::kDouble>(v->get_variant()))
        return fail("2.0 did not round-trip as kDouble (lossy bridge!)");
    if (ext::value_as_double(*v).value_or(0.0) != 2.0) return fail("double value wrong");
    return true;
}

// Binary must survive; the JSON bridge would turn it into a list of integers.
bool test_binary_fidelity() {
    using loro::LoroValue;
    auto doc = loro::LoroDoc::init();
    auto map = doc->get_map(root("m"));
    std::vector<uint8_t> bytes{0x00, 0x01, 0xff, 0x7f, 0x80};
    map->insert("b", ext::value_like_from(bytes));

    auto v = map->get("b")->as_value();
    if (!v.has_value()) return fail("binary get returned no value");
    if (!std::holds_alternative<LoroValue::kBinary>(v->get_variant()))
        return fail("binary did not round-trip as kBinary (lossy bridge!)");
    auto got = ext::value_as_binary(*v);
    if (!got.has_value() || *got != bytes) return fail("binary bytes wrong");
    return true;
}

// The new (Phase 2) LoroList typed path must preserve the same distinctions the JSON bridge
// would corrupt: 2.0 stays kDouble (not kI64) and binary stays kBinary (not a kList).
bool test_list_value_fidelity() {
    using loro::LoroValue;
    auto doc = loro::LoroDoc::init();
    auto list = doc->get_list(root("l"));
    std::vector<uint8_t> bytes{0x00, 0x10, 0xff, 0x7f};
    list->push(ext::value_like_from(2.0));
    list->push(ext::value_like_from(bytes));

    auto d = list->get(0)->as_value();
    if (!d.has_value() || !std::holds_alternative<LoroValue::kDouble>(d->get_variant()))
        return fail("list 2.0 did not round-trip as kDouble (lossy bridge!)");
    if (ext::value_as_double(*d).value_or(0.0) != 2.0) return fail("list double value wrong");

    auto b = list->get(1)->as_value();
    if (!b.has_value() || !std::holds_alternative<LoroValue::kBinary>(b->get_variant()))
        return fail("list binary did not round-trip as kBinary (lossy bridge!)");
    auto got = ext::value_as_binary(*b);
    if (!got.has_value() || *got != bytes) return fail("list binary bytes wrong");

    // to_vec() must preserve the same typed kinds (it walks loro_list_get_value).
    auto vec = list->to_vec();
    if (vec.size() != 2) return fail("list to_vec size != 2");
    if (!std::holds_alternative<LoroValue::kDouble>(vec[0].get_variant()))
        return fail("to_vec[0] not kDouble");
    if (!std::holds_alternative<LoroValue::kBinary>(vec[1].get_variant()))
        return fail("to_vec[1] not kBinary");
    return true;
}

// RESHAPE Phase 4: import() returns a real ImportStatus (per-peer success spans) — the Rust
// layer used to discard it (it returned an empty struct). loro-cpp's own tests never inspect it.
bool test_import_status_success() {
    auto doc1 = loro::LoroDoc::init();
    doc1->set_peer_id(1);
    auto text = doc1->get_text(root("body"));
    text->insert(0, "hello world");
    doc1->commit();
    auto snapshot = doc1->export_snapshot();

    auto doc2 = loro::LoroDoc::init();
    doc2->set_peer_id(2);
    auto status = doc2->import(snapshot);

    auto it = status.success.find(1);
    if (it == status.success.end()) return fail("import status: peer 1 missing from success");
    if (it->second.end <= it->second.start)
        return fail("import status: peer 1 success span is empty/invalid");
    // A self-contained snapshot has no missing deps: pending must be absent or empty.
    if (status.pending.has_value() && !status.pending->empty())
        return fail("import status: unexpected pending entries for a complete snapshot");
    return true;
}

// Importing updates whose dependencies are missing must surface a non-empty `pending` map.
bool test_import_status_pending() {
    auto doc1 = loro::LoroDoc::init();
    doc1->set_peer_id(1);
    auto text = doc1->get_text(root("body"));
    text->insert(0, "a");
    doc1->commit();
    auto vv_after_first = doc1->state_vv();
    text->insert(text->len_unicode(), "b");
    doc1->commit();
    // Only the second commit's ops — they depend on the (un-imported) first commit.
    auto update2 = doc1->export_updates(vv_after_first);

    auto doc2 = loro::LoroDoc::init();
    doc2->set_peer_id(2);
    auto status = doc2->import(update2);
    if (!status.pending.has_value())
        return fail("import status: expected a pending map for updates with missing deps");
    if (status.pending->find(1) == status.pending->end())
        return fail("import status: peer 1 missing from pending");
    return true;
}

// RESHAPE Phase 4: the undo meta value crosses via the TYPED ABI, so a binary value an on_push
// stores is read back intact by on_pop (the old JSON bridge would corrupt it into a list).
bool test_undo_meta_value_fidelity() {
    using loro::LoroValue;
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(7);
    auto undo = loro::UndoManager::init(doc);

    std::vector<uint8_t> stored{0x00, 0x01, 0xff, 0x7f, 0x80};
    std::optional<LoroValue> popped;
    ext::set_on_push(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                                std::optional<loro::DiffEvent>) {
        return loro::UndoItemMeta{ext::value_from(stored), {}};
    });
    ext::set_on_pop(*undo, [&](const loro::UndoOrRedo &, const loro::CounterSpan &,
                               const loro::UndoItemMeta &meta) { popped = meta.value; });

    auto text = doc->get_text(root("body"));
    text->insert(0, "x");
    doc->commit();
    undo->record_new_checkpoint();
    text->insert(text->len_unicode(), "y");
    doc->commit();
    undo->record_new_checkpoint();

    if (!undo->undo()) return fail("undo() returned false");
    if (!popped.has_value()) return fail("on_pop did not deliver a meta value");
    if (!std::holds_alternative<LoroValue::kBinary>(popped->get_variant()))
        return fail("undo meta value did not round-trip as kBinary (lossy bridge!)");
    auto got = ext::value_as_binary(*popped);
    if (!got.has_value() || *got != stored) return fail("undo meta binary bytes wrong");
    return true;
}

bool run() {
    if (!test_value_helpers()) return false;
    if (!test_double_fidelity()) return false;
    if (!test_binary_fidelity()) return false;
    if (!test_list_value_fidelity()) return false;
    if (!test_import_status_success()) return false;
    if (!test_import_status_pending()) return false;
    if (!test_undo_meta_value_fidelity()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
