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
#include <variant>

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

// Records every path node seen, so the test can check the peer round-trips through the
// path JSON parser.
class PathNodeCollector : public loro::Subscriber {
public:
    int count = 0;
    std::vector<loro::TreeId> nodes;
    void on_diff(const loro::DiffEvent &diff) override {
        ++count;
        for (const auto &cd : diff.events) {
            for (const auto &step : cd.path) {
                if (auto *n = std::get_if<loro::Index::kNode>(&step.index.get_variant())) {
                    nodes.push_back(n->target);
                }
            }
        }
    }
};

// Regression for gsfjohnson/loro-c#6: a peer id with bit 63 set used to make the path JSON
// parser throw (stoll overflow) inside the subscriber trampoline, which swallowed the exception
// and silently dropped every tree diff event. Map/list/text events were unaffected, and
// LoroDoc::init() picks a random 64-bit peer, so half of all fresh docs hit this.
bool large_peer_tree_events() {
    const uint64_t peers[] = {1ULL, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
                              0xfffffffffffffffeULL, 12012086296529505043ULL};
    for (uint64_t peer : peers) {
        auto doc = loro::LoroDoc::init();
        doc->set_peer_id(peer);
        auto tree = doc->get_tree(root("tree"));
        auto sub = std::make_shared<PathNodeCollector>();
        auto handle = doc->subscribe_root(sub);

        auto node = tree->create(loro::TreeParentId(loro::TreeParentId::kRoot{}));
        tree->get_meta(node)->insert("name", str_value("A"));
        doc->commit();

        if (sub->count == 0) {
            std::cerr << "peer=" << peer << ": ";
            return fail("no tree diff event delivered (loro-c#6 regression)");
        }
        if (node.peer != peer) {
            std::cerr << "peer=" << peer << ": ";
            return fail("created TreeId does not carry the doc's peer");
        }
        bool saw_node = false;
        for (const auto &n : sub->nodes) {
            if (n.peer == peer && n.counter == node.counter) saw_node = true;
        }
        if (!saw_node) {
            std::cerr << "peer=" << peer << ": ";
            return fail("path node peer did not round-trip through the path JSON parser");
        }
        handle->unsubscribe();
    }
    return true;
}

class DiffCollector : public loro::Subscriber {
public:
    std::vector<loro::DiffEvent> events;
    void on_diff(const loro::DiffEvent &diff) override { events.push_back(diff); }
};

bool same_container(const loro::ContainerId &a, const loro::ContainerId &b) {
    const auto &x = a.get_variant();
    const auto &y = b.get_variant();
    if (x.index() != y.index()) return false;
    if (auto *r = std::get_if<loro::ContainerId::kRoot>(&x)) {
        return r->name == std::get<loro::ContainerId::kRoot>(y).name;
    }
    const auto &n = std::get<loro::ContainerId::kNormal>(x);
    const auto &m = std::get<loro::ContainerId::kNormal>(y);
    return n.peer == m.peer && n.counter == m.counter;
}

bool key_is(const loro::Index &i, const std::string &key) {
    auto *k = std::get_if<loro::Index::kKey>(&i.get_variant());
    return k && k->key == key;
}

bool seq_is(const loro::Index &i, uint32_t index) {
    auto *s = std::get_if<loro::Index::kSeq>(&i.get_variant());
    return s && s->index == index;
}

// A root subscriber runs inside import(), before it returns, and each diff carries its path
// from the root. A cache keyed by top-level block can then find the block an imported edit
// touched: the path item whose index is a kSeq under the body list.
bool import_events_are_synchronous_with_paths() {
    auto a = loro::LoroDoc::init();
    a->set_peer_id(1);
    auto note = a->get_map(root("note"));
    auto body = note->insert_movable_list_container("body", loro::LoroMovableList::init());
    std::vector<std::shared_ptr<loro::LoroMap>> blocks;
    std::vector<std::shared_ptr<loro::LoroText>> contents;
    for (uint32_t i = 0; i < 2; ++i) {
        blocks.push_back(body->insert_map_container(i, loro::LoroMap::init()));
        contents.push_back(blocks[i]->insert_text_container("content", loro::LoroText::init()));
        contents[i]->insert(0, "block");
    }
    a->commit();

    auto b = loro::LoroDoc::init();
    b->set_peer_id(2);
    b->import(a->export_snapshot());
    auto collector = std::make_shared<DiffCollector>();
    auto handle = b->subscribe_root(collector);

    contents[1]->insert(5, "!");
    a->commit();
    b->import(a->export_updates(b->oplog_vv()));

    if (collector->events.size() != 1) {
        return fail("import did not deliver one event before returning");
    }
    const loro::DiffEvent &ev = collector->events[0];
    if (ev.triggered_by != loro::EventTriggerKind::kImport) {
        return fail("event not triggered by import");
    }
    if (ev.events.size() != 1) return fail("import of one text edit gave other than one diff");
    const loro::ContainerDiff &d = ev.events[0];
    if (!same_container(d.target, contents[1]->id())) {
        return fail("diff target is not the edited text");
    }

    const auto &p = d.path;
    bool shape = p.size() == 4 && same_container(p[0].container, note->id()) &&
                 key_is(p[0].index, "note") && same_container(p[1].container, body->id()) &&
                 key_is(p[1].index, "body") && same_container(p[2].container, blocks[1]->id()) &&
                 seq_is(p[2].index, 1) && same_container(p[3].container, contents[1]->id()) &&
                 key_is(p[3].index, "content");
    if (!shape) {
        for (const auto &item : p) {
            const auto &idx = item.index.get_variant();
            std::cerr << "  path item: index variant " << idx.index();
            if (auto *k = std::get_if<loro::Index::kKey>(&idx)) std::cerr << " key " << k->key;
            if (auto *s = std::get_if<loro::Index::kSeq>(&idx)) std::cerr << " seq " << s->index;
            std::cerr << "\n";
        }
        return fail("path is not note/body/block[1]/content");
    }
    handle->unsubscribe();
    return true;
}

bool run() {
    if (!large_peer_tree_events()) return false;
    if (!import_events_are_synchronous_with_paths()) return false;

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
