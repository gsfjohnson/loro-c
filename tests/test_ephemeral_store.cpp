// Exercises EphemeralStore: set / get / keys / delete / encode_all / apply,
// the EphemeralSubscriber callback, and the ext lambda subscribe overloads.
#include "test_helpers.hpp"

#include <loro/loro_ext.hpp>

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

    auto all = store->get_all_states();
    if (all.size() != 2) return fail("get_all_states should return 2 entries");
    auto it_name = all.find("name");
    if (it_name == all.end() || loro_value_as_string(it_name->second) != "alice") {
        return fail("get_all_states missing/incorrect 'name'");
    }
    auto it_cursor = all.find("cursor");
    if (it_cursor == all.end() || loro_value_as_i64(it_cursor->second) != 42) {
        return fail("get_all_states missing/incorrect 'cursor'");
    }

    // remove_outdated with a 30s timeout drops nothing (entries are fresh); must not throw.
    store->remove_outdated();
    if (store->keys().size() != 2) return fail("remove_outdated dropped fresh entries");

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

    // --- ext lambda subscribe overloads (UPVERT_3) ---
    {
        auto s = loro::EphemeralStore::init(30'000);

        std::atomic<int> ev_count{0};
        std::vector<std::string> seen_keys;
        auto ev_sub = loro::ext::subscribe(*s, [&](const loro::EphemeralStoreEvent &e) {
            ev_count.fetch_add(1);
            for (const auto &k : e.added) seen_keys.push_back(k);
            for (const auto &k : e.removed) seen_keys.push_back(k);
        });

        std::atomic<int> lu_count{0};
        std::vector<uint8_t> last_update;
        auto lu_sub = loro::ext::subscribe_local_update(
            *s, [&](const std::vector<uint8_t> &bytes) {
                lu_count.fetch_add(1);
                if (!bytes.empty()) last_update = bytes;
            });

        s->set("presence", str_value("online"));
        s->delete_("presence");

        if (ev_count.load() == 0) return fail("ext::subscribe(store) lambda never fired");
        bool saw_presence = false;
        for (const auto &k : seen_keys) {
            if (k == "presence") saw_presence = true;
        }
        if (!saw_presence) return fail("ext ephemeral event missing 'presence'");

        if (lu_count.load() == 0) {
            return fail("ext::subscribe_local_update(store) lambda never fired");
        }
        if (last_update.empty()) return fail("ext local-update bytes were empty");

        ev_sub->unsubscribe();
        lu_sub->unsubscribe();
    }

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
