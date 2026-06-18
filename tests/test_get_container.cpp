// Exercises LoroDoc::get_*(ContainerId) for NESTED (normal) container ids — issue #2 blocker.
//
// Pre-fix, resolving any non-root container id threw "conformance spike: only root (by-name)
// ContainerId is supported here". The fix routes nested ids through the non-creating
// loro_doc_try_get_* lookups, while roots keep the get-or-create by-name path.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

// A ContainerIdLike that hands back the exact ContainerId it holds (root OR nested), ignoring
// the type the resolver requests. The shared StringContainerId helper can only make root ids,
// so nested-id resolution needs this adapter.
class ExactContainerId : public loro::ContainerIdLike {
public:
    explicit ExactContainerId(loro::ContainerId id) : id_(std::move(id)) {}
    loro::ContainerId as_container_id(loro::ContainerType) override { return id_; }

private:
    loro::ContainerId id_;
};

std::shared_ptr<loro::ContainerIdLike> exact(loro::ContainerId id) {
    return std::make_shared<ExactContainerId>(std::move(id));
}

bool is_normal(const loro::ContainerId &cid) {
    return std::holds_alternative<loro::ContainerId::kNormal>(cid.get_variant());
}

bool run() {
    auto doc = loro::LoroDoc::init();

    // (5) Root regression: get_text(root(...)) on a fresh doc get-or-creates a usable handle.
    auto title = doc->get_text(root("title"));
    title->insert(0, "hi");
    if (title->to_string() != "hi") return fail("root get_text regression");

    // (1) Nested text under a map: resolve it from its stored ContainerId.
    auto map = doc->get_map(root("settings"));
    auto attached_text = map->insert_text_container("body", loro::LoroText::init());
    attached_text->insert(0, "hello");

    auto body = map->get("body")->as_loro_text();
    loro::ContainerId text_cid = body->id();
    if (!is_normal(text_cid)) return fail("nested text id should be a normal (non-root) id");

    auto resolved_text = doc->get_text(exact(text_cid));  // pre-fix: throws
    if (resolved_text->to_string() != "hello") {
        return fail("get_text(nested cid) readback wrong");
    }

    // (2) Nested map under a list: resolve it from its stored ContainerId.
    auto list = doc->get_list(root("blocks"));
    auto attached_map = list->insert_map_container(0, loro::LoroMap::init());
    attached_map->insert("k", str_value("v"));

    auto nested_map = list->get(0)->as_loro_map();
    loro::ContainerId map_cid = nested_map->id();
    if (!is_normal(map_cid)) return fail("nested map id should be a normal id");

    auto resolved_map = doc->get_map(exact(map_cid));  // pre-fix: throws
    auto kv = resolved_map->get("k")->as_value();
    if (!kv.has_value() || loro_value_as_string(*kv) != "v") {
        return fail("get_map(nested cid) readback wrong");
    }

    // (3) cid-string round-trip guards the serializer the nested branch depends on.
    std::string s = loro::detail::container_id_to_cid_string(text_cid);
    loro::ContainerId reparsed = loro::detail::cid_string_to_container_id(s);
    auto resolved_again = doc->get_text(exact(reparsed));
    if (resolved_again->to_string() != "hello") return fail("cid round-trip resolution wrong");

    // (4) Absent nested id: the non-creating lookup must throw, not silently create.
    loro::ContainerId absent(loro::ContainerId::kNormal{
        /*peer*/ 123456789ULL, /*counter*/ 4242,
        loro::ContainerType(loro::ContainerType::kText{})});
    bool threw_absent = false;
    try {
        doc->get_text(exact(absent));
    } catch (const loro::LoroError &) {
        threw_absent = true;
    }
    if (!threw_absent) return fail("get_text(absent nested id) should throw");

    // (6) Type mismatch: resolving a Text cid as a map must throw.
    bool threw_mismatch = false;
    try {
        doc->get_map(exact(text_cid));
    } catch (const loro::LoroError &) {
        threw_mismatch = true;
    }
    if (!threw_mismatch) return fail("get_map(text cid) should throw on type mismatch");

    return true;
}

} // namespace

LORO_TEST_MAIN(run)
