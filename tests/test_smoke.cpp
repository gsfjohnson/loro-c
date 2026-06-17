// M1 smoke test: confirms the generated bindings can drive a full
// LoroDoc → LoroText edit → export_snapshot → import-into-fresh-doc
// → readback round-trip end-to-end.
//
// This is the minimum viable end-to-end exercise; M2 expands to one
// test per UDL interface.

#include <loro.hpp>

#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace {

// ContainerIdLike is a [Trait, WithForeign] interface in the UDL. The C++
// equivalent is a pure virtual base — implement it here so the test can
// pass plain string-named root containers to LoroDoc::get_text().
// (M3 will wrap this pattern in an ergonomics layer.)
class StringContainerId : public loro::ContainerIdLike {
public:
    explicit StringContainerId(std::string name) : name_(std::move(name)) {}

    loro::ContainerId as_container_id(loro::ContainerType ty) override {
        return loro::ContainerId(loro::ContainerId::kRoot{name_, ty});
    }

private:
    std::string name_;
};

bool fail(const char *msg) {
    std::cerr << "FAIL: " << msg << "\n";
    return false;
}

bool run() {
    auto doc1 = loro::LoroDoc::init();
    auto id1  = std::make_shared<StringContainerId>("text");
    auto text = doc1->get_text(id1);

    text->insert(0, "hello");
    text->insert(5, " world");

    if (text->to_string() != "hello world") {
        return fail("doc1 text != \"hello world\"");
    }
    if (text->len_unicode() != 11) {
        return fail("doc1 text length != 11");
    }

    auto snapshot = doc1->export_snapshot();
    if (snapshot.empty()) {
        return fail("snapshot was empty");
    }

    auto doc2 = loro::LoroDoc::init();
    doc2->import(snapshot);
    auto text2 = doc2->get_text(std::make_shared<StringContainerId>("text"));

    if (text2->to_string() != "hello world") {
        return fail("doc2 text != \"hello world\"");
    }
    if (text2->len_unicode() != 11) {
        return fail("doc2 text length != 11");
    }

    return true;
}

} // namespace

int main() {
    try {
        return run() ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: exception: " << e.what() << "\n";
        return 2;
    } catch (...) {
        std::cerr << "FAIL: unknown exception\n";
        return 2;
    }
}
