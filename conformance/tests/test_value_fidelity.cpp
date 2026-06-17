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

bool run() {
    if (!test_value_helpers()) return false;
    if (!test_double_fidelity()) return false;
    if (!test_binary_fidelity()) return false;
    return true;
}

}  // namespace

LORO_TEST_MAIN(run)
