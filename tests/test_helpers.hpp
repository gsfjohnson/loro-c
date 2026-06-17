// Shared helpers for M2 surface tests.
//
// Two adapters appear repeatedly across the loro-ffi UDL surface:
//   - ContainerIdLike — pure-virtual base wrapping a root-container name
//   - LoroValueLike   — pure-virtual base wrapping a primitive LoroValue
//
// M3 will fold these into the public ergonomics layer; for now the tests
// instantiate them directly so the generated bindings are exercised as-is.
#pragma once

#include <loro.hpp>

#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace loro_test {

class StringContainerId : public loro::ContainerIdLike {
public:
    explicit StringContainerId(std::string name) : name_(std::move(name)) {}

    loro::ContainerId as_container_id(loro::ContainerType ty) override {
        return loro::ContainerId(loro::ContainerId::kRoot{name_, ty});
    }

private:
    std::string name_;
};

inline std::shared_ptr<loro::ContainerIdLike> root(std::string name) {
    return std::make_shared<StringContainerId>(std::move(name));
}

class PrimitiveValue : public loro::LoroValueLike {
public:
    explicit PrimitiveValue(loro::LoroValue v) : value_(std::move(v)) {}

    loro::LoroValue as_loro_value() override { return value_; }

private:
    loro::LoroValue value_;
};

inline std::shared_ptr<loro::LoroValueLike> str_value(std::string s) {
    return std::make_shared<PrimitiveValue>(
        loro::LoroValue(loro::LoroValue::kString{std::move(s)}));
}

inline std::shared_ptr<loro::LoroValueLike> i64_value(int64_t v) {
    return std::make_shared<PrimitiveValue>(
        loro::LoroValue(loro::LoroValue::kI64{v}));
}

inline std::shared_ptr<loro::LoroValueLike> bool_value(bool v) {
    return std::make_shared<PrimitiveValue>(
        loro::LoroValue(loro::LoroValue::kBool{v}));
}

inline std::shared_ptr<loro::LoroValueLike> double_value(double v) {
    return std::make_shared<PrimitiveValue>(
        loro::LoroValue(loro::LoroValue::kDouble{v}));
}

inline std::shared_ptr<loro::LoroValueLike> null_value() {
    return std::make_shared<PrimitiveValue>(
        loro::LoroValue(loro::LoroValue::kNull{}));
}

inline std::string loro_value_as_string(const loro::LoroValue &v) {
    if (auto *s = std::get_if<loro::LoroValue::kString>(&v.get_variant())) {
        return s->value;
    }
    return "<not a string>";
}

inline int64_t loro_value_as_i64(const loro::LoroValue &v) {
    if (auto *i = std::get_if<loro::LoroValue::kI64>(&v.get_variant())) {
        return i->value;
    }
    return 0;
}

inline bool fail(const char *msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return false;
}

} // namespace loro_test

#define LORO_TEST_MAIN(fn)                                                   \
    int main() {                                                             \
        try {                                                                \
            return (fn)() ? 0 : 1;                                           \
        } catch (const std::exception &e) {                                  \
            std::cerr << "FAIL: exception: " << e.what() << "\n";            \
            return 2;                                                        \
        } catch (...) {                                                      \
            std::cerr << "FAIL: unknown exception\n";                        \
            return 2;                                                        \
        }                                                                    \
    }
