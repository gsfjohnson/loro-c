// G5: value navigation — get_by_str_path / get_by_path resolve a document path to either a
// plain value or a LIVE nested container. The headline assertion is that a container obtained by
// path is live: mutating it is observable on the parent document, which a flat JSON dump cannot
// represent.

#include <loro/loro.hpp>

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

// Builds a doc whose root list "items" holds one map: items[0] = { "name": "alice" }.
static loro::Doc make_doc() {
    loro::Doc doc;
    loro::List items = doc.get_list("items");
    loro::Container child = items.push_container(loro::Container::map());
    child.as_map().insert("name", "\"alice\"");
    doc.commit();
    return doc;
}

// get_by_str_path returns a live container; writing through it shows up on the parent document.
static void test_navigate_to_live_container() {
    loro::Doc doc = make_doc();

    auto voc = doc.get_by_str_path("items/0");
    CHECK(voc.has_value());
    CHECK(voc->is_container());
    CHECK(voc->container_type() == LORO_CONTAINER_MAP);

    auto child = voc->container();
    CHECK(child.has_value());
    loro::Map live = child->as_map();
    live.insert("age", "30");
    doc.commit();

    const std::string dj = doc.to_json();
    CHECK(contains(dj, "age"));
    CHECK(contains(dj, "30"));
}

// A leaf resolves to a plain value: not a container, readable as JSON.
static void test_navigate_to_value() {
    loro::Doc doc = make_doc();

    auto voc = doc.get_by_str_path("items/0/name");
    CHECK(voc.has_value());
    CHECK(!voc->is_container());
    CHECK(voc->container_type() == LORO_CONTAINER_UNKNOWN);
    CHECK(!voc->container().has_value());
    CHECK(contains(voc->value_json(), "alice"));
}

// The structured path form reaches the same node as the string form.
static void test_get_by_path_components() {
    loro::Doc doc = make_doc();

    std::vector<loro::PathComponent> path{loro::PathComponent::key("items"),
                                          loro::PathComponent::seq(0)};
    auto voc = doc.get_by_path(path);
    CHECK(voc.has_value());
    CHECK(voc->is_container());
    CHECK(voc->container_type() == LORO_CONTAINER_MAP);
}

// A path that does not resolve yields std::nullopt (no exception).
static void test_missing_path() {
    loro::Doc doc = make_doc();
    CHECK(!doc.get_by_str_path("items/99").has_value());
    CHECK(!doc.get_by_str_path("nope/0").has_value());
}

int main() {
    test_navigate_to_live_container();
    test_navigate_to_value();
    test_get_by_path_components();
    test_missing_path();

    if (failures == 0) {
        std::puts("test_navigate: OK");
        return 0;
    }
    std::fprintf(stderr, "test_navigate: %d failure(s)\n", failures);
    return 1;
}
