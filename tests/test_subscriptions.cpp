// Exercises the subscription / event surface end-to-end:
//   - LoroDoc::subscribe (per-container) + Subscription::unsubscribe
//   - LoroDoc::subscribe_root
//   - LoroDoc::subscribe_local_update
//   - LoroDoc::subscribe_first_commit_from_peer
//   - LoroDoc::subscribe_pre_commit  (with ChangeModifier mutation)
//
// Each callback type in the generated bindings is a pure-virtual interface;
// these subclasses are the test's stand-ins for the std::function adapters
// that M3 will provide.
#include "test_helpers.hpp"

#include <atomic>

using namespace loro_test;

namespace {

class CountingSubscriber : public loro::Subscriber {
public:
    std::atomic<int> count{0};
    std::optional<loro::ContainerId> last_target;
    void on_diff(const loro::DiffEvent &diff) override {
        count.fetch_add(1);
        last_target = diff.current_target;
    }
};

class CountingLocalUpdate : public loro::LocalUpdateCallback {
public:
    std::atomic<int> count{0};
    std::vector<uint8_t> last_update;
    void on_local_update(const std::vector<uint8_t> &update) override {
        count.fetch_add(1);
        last_update = update;
    }
};

class CountingFirstCommit : public loro::FirstCommitFromPeerCallback {
public:
    std::atomic<int> count{0};
    std::vector<uint64_t> peers;
    void on_first_commit_from_peer(const loro::FirstCommitFromPeerPayload &p) override {
        count.fetch_add(1);
        peers.push_back(p.peer);
    }
};

class MessageInjector : public loro::PreCommitCallback {
public:
    std::atomic<int> count{0};
    void on_pre_commit(const loro::PreCommitCallbackPayload &p) override {
        count.fetch_add(1);
        p.modifier->set_message("injected");
    }
};

bool run() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(11);
    doc->set_record_timestamp(true);

    auto text = doc->get_text(root("body"));
    auto cid = text->id();

    auto text_sub = std::make_shared<CountingSubscriber>();
    auto sub = doc->subscribe(cid, text_sub);

    auto root_sub = std::make_shared<CountingSubscriber>();
    auto sub_root = doc->subscribe_root(root_sub);

    auto local_cb = std::make_shared<CountingLocalUpdate>();
    auto sub_local = doc->subscribe_local_update(local_cb);

    auto first_cb = std::make_shared<CountingFirstCommit>();
    auto sub_first = doc->subscribe_first_commit_from_peer(first_cb);

    auto pre_cb = std::make_shared<MessageInjector>();
    auto sub_pre = doc->subscribe_pre_commit(pre_cb);

    text->insert(0, "ping");
    doc->commit();

    if (text_sub->count.load() == 0) return fail("container subscriber did not fire");
    if (root_sub->count.load() == 0) return fail("root subscriber did not fire");
    if (local_cb->count.load() == 0) return fail("local-update callback did not fire");
    if (local_cb->last_update.empty()) return fail("local-update payload empty");
    if (first_cb->count.load() == 0) return fail("first-commit-from-peer callback did not fire");
    if (pre_cb->count.load() == 0) return fail("pre-commit callback did not fire");

    auto changes = doc->get_changed_containers_in(loro::Id{11, 0}, /*len=*/1);
    bool change_message_set = false;
    for (uint32_t i = 0; i < doc->len_changes(); i++) {
        auto change = doc->get_change(loro::Id{11, static_cast<int32_t>(i)});
        if (change.has_value() && change->message.has_value() &&
            *change->message == "injected") {
            change_message_set = true;
            break;
        }
    }
    if (!change_message_set) {
        return fail("ChangeModifier::set_message did not stick");
    }

    int container_count_before = text_sub->count.load();
    sub->unsubscribe();
    text->insert(text->len_unicode(), " pong");
    doc->commit();
    if (text_sub->count.load() != container_count_before) {
        return fail("unsubscribed container subscriber still fired");
    }
    if (root_sub->count.load() <= 0) {
        return fail("root subscriber should still be receiving events");
    }

    sub_root->unsubscribe();
    sub_local->unsubscribe();
    sub_first->unsubscribe();
    sub_pre->unsubscribe();
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
