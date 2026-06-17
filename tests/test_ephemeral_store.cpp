// Exercises EphemeralStore: set / get / keys / delete / encode_all / apply
// and the EphemeralSubscriber callback.
#include "test_helpers.hpp"

#include <atomic>

using namespace loro_test;

namespace {

class CountingSubscriber : public loro::EphemeralSubscriber {
public:
    std::atomic<int> events{0};
    std::vector<std::string> last_added;
    std::vector<std::string> last_updated;
    std::vector<std::string> last_removed;

    void on_ephemeral_event(const loro::EphemeralStoreEvent &e) override {
        events.fetch_add(1);
        last_added = e.added;
        last_updated = e.updated;
        last_removed = e.removed;
    }
};

bool run() {
    auto store = loro::EphemeralStore::init(/*timeout_ms=*/30'000);

    auto sub = std::make_shared<CountingSubscriber>();
    auto subscription = store->subscribe(sub);

    store->set("cursor", i64_value(42));
    store->set("name", str_value("alice"));

    auto v = store->get("name");
    if (!v.has_value() || loro_value_as_string(*v) != "alice") {
        return fail("ephemeral get('name') wrong");
    }

    auto keys = store->keys();
    if (keys.size() != 2) return fail("expected 2 keys in store");

    auto encoded = store->encode_all();
    if (encoded.empty()) return fail("encode_all returned empty");

    auto store2 = loro::EphemeralStore::init(30'000);
    store2->apply(encoded);
    auto v2 = store2->get("cursor");
    if (!v2.has_value() || loro_value_as_i64(*v2) != 42) {
        return fail("apply did not transfer 'cursor'");
    }

    int before = sub->events.load();
    store->delete_("name");
    if (sub->events.load() <= before) {
        return fail("subscriber did not see delete");
    }
    bool saw_removed = false;
    for (const auto &k : sub->last_removed) {
        if (k == "name") saw_removed = true;
    }
    if (!saw_removed) return fail("event did not list 'name' as removed");

    subscription->unsubscribe();
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
