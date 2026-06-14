// M3: subscriptions & DiffEvent marshalling via the C++ wrapper.
//   - subscribe_root / subscribe (per-container) / subscribe_local_update
//   - DiffEvent envelope (trigger kind, target, diff kind) + JSON delta payload
//   - unsubscribe on Subscription destruction
//   - free_user_data (the captured std::function) runs exactly once on unsubscribe

#include <loro/loro.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)

// A subscribe_root edit fires once with a structured envelope + JSON text delta.
static void test_subscribe_root() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");

    int calls = 0;
    loro::EventTriggerKind trigger{};
    std::size_t count = 0;
    std::string target;
    loro::DiffKind kind{};
    std::string json;

    auto sub = doc.subscribe_root([&](const loro::DiffEvent& ev) {
        ++calls;
        trigger = ev.triggered_by();
        count = ev.size();
        if (ev.size() > 0) {
            loro::ContainerDiff d = ev[0];
            target = d.target();
            kind = d.kind();
            json = d.to_json();
        }
    });

    t.insert(0, "hello");
    doc.commit();

    CHECK(calls == 1);
    CHECK(trigger == LORO_EVENT_TRIGGER_LOCAL);
    CHECK(count >= 1);
    CHECK(target == t.id());
    CHECK(kind == LORO_DIFF_TEXT);
    CHECK(json.find("hello") != std::string::npos);
}

// A per-container subscription fires only for its own container's changes.
static void test_subscribe_container() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");
    loro::Map m = doc.get_map("m");

    int calls = 0;
    std::string seen_target;
    auto sub = doc.subscribe(t.id(), [&](const loro::DiffEvent& ev) {
        ++calls;
        if (ev.size() > 0) seen_target = ev[0].target();
    });

    t.insert(0, "hi");
    doc.commit();
    CHECK(calls == 1);
    CHECK(seen_target == t.id());

    // A commit that touches only a different container must not fire this subscription.
    m.insert("k", "1");
    doc.commit();
    CHECK(calls == 1);
}

// subscribe_local_update delivers importable update bytes for each local commit.
static void test_subscribe_local_update() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");

    int calls = 0;
    std::vector<std::uint8_t> update;
    auto sub = doc.subscribe_local_update([&](const std::uint8_t* data, std::size_t len) -> bool {
        ++calls;
        update.assign(data, data + len);
        return true;
    });

    t.insert(0, "hello");
    doc.commit();
    CHECK(calls == 1);
    CHECK(!update.empty());

    // The update bytes alone reconstruct the change in a fresh document.
    loro::Doc fresh;
    fresh.import(update);
    CHECK(fresh.get_text("t").to_string() == "hello");
}

// Destroying the Subscription unsubscribes: no further callbacks fire.
static void test_unsubscribe() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");

    int calls = 0;
    {
        auto sub = doc.subscribe_root([&](const loro::DiffEvent&) { ++calls; });
        t.insert(0, "a");
        doc.commit();
        CHECK(calls == 1);
    }  // sub destroyed here -> unsubscribed

    t.insert(1, "b");
    doc.commit();
    CHECK(calls == 1);
}

// The captured callback (and its user data) is released exactly once on unsubscribe.
static void test_free_user_data() {
    loro::Doc doc;
    auto probe = std::make_shared<int>(0);
    CHECK(probe.use_count() == 1);
    {
        auto sub = doc.subscribe_root([probe](const loro::DiffEvent&) { (void)probe; });
        // The heap-allocated std::function holds a copy of the captured shared_ptr.
        CHECK(probe.use_count() >= 2);
    }  // sub destroyed -> free_user_data deletes the std::function -> the copy is released
    CHECK(probe.use_count() == 1);
}

// detach() leaves the callback firing (until the doc is dropped) and empties the handle.
static void test_detach() {
    loro::Doc doc;
    loro::Text t = doc.get_text("t");

    int calls = 0;
    auto sub = doc.subscribe_root([&](const loro::DiffEvent&) { ++calls; });
    sub.detach();
    CHECK(sub.raw() == nullptr);

    t.insert(0, "x");
    doc.commit();
    CHECK(calls == 1);  // still firing after detach
}

int main() {
    test_subscribe_root();
    test_subscribe_container();
    test_subscribe_local_update();
    test_unsubscribe();
    test_free_user_data();
    test_detach();

    if (failures == 0) {
        std::puts("test_subscriptions: OK");
        return 0;
    }
    std::fprintf(stderr, "test_subscriptions: %d failure(s)\n", failures);
    return 1;
}
