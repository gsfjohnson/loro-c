// M2: LoroMap and LoroList — JSON value round-trips, nested containers (a map inside a
// list, and a list inside a map), snapshot round trip, and strong-co-ownership ordering.

#include <loro/loro.hpp>

#include <cstdio>
#include <memory>
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

static void test_map_basics() {
    loro::Doc doc;
    loro::Map m = doc.get_map("m");
    CHECK(m.empty());

    m.insert("n", "42");
    m.insert("s", "\"hello\"");
    m.insert("b", "true");
    doc.commit();

    CHECK(!m.empty());
    CHECK(m.size() == 3);
    CHECK(m.get("n") == "42");
    CHECK(m.get("s") == "\"hello\"");
    CHECK(m.get("b") == "true");
    CHECK(m.get("missing") == std::nullopt);

    m.remove("b");
    doc.commit();
    CHECK(m.size() == 2);
    CHECK(m.get("b") == std::nullopt);

    m.clear();
    doc.commit();
    CHECK(m.empty());
}

static void test_list_basics() {
    loro::Doc doc;
    loro::List l = doc.get_list("l");
    CHECK(l.empty());

    l.push("1");
    l.push("2");
    l.insert(0, "0");  // -> [0, 1, 2]
    doc.commit();

    CHECK(l.size() == 3);
    CHECK(l.get(0) == "0");
    CHECK(l.get(2) == "2");
    CHECK(l.get(3) == std::nullopt);  // out of range

    std::optional<std::string> popped = l.pop();  // removes the "2"
    CHECK(popped == "2");
    doc.commit();
    CHECK(l.size() == 2);

    l.remove(0, 1);  // drop the head
    doc.commit();
    CHECK(l.size() == 1);
    CHECK(l.get(0) == "1");
}

// A map stored inside a list, attached via insert_container and read back via
// get_container. Exercises the type-erased Container round trip.
static void test_nested_map_in_list() {
    loro::Doc doc;
    loro::List l = doc.get_list("l");

    loro::Container attached = l.push_container(loro::Container::map());
    CHECK(attached.type() == LORO_CONTAINER_MAP);
    loro::Map child = attached.as_map();
    child.insert("name", "\"alice\"");
    child.insert("age", "30");
    doc.commit();

    CHECK(l.size() == 1);
    std::optional<loro::Container> got = l.get_container(0);
    CHECK(got.has_value());
    CHECK(got->type() == LORO_CONTAINER_MAP);
    loro::Map readback = got->as_map();
    CHECK(readback.get("name") == "\"alice\"");
    CHECK(readback.get("age") == "30");

    // A plain value is not a container.
    l.push("99");
    doc.commit();
    CHECK(l.get_container(1) == std::nullopt);
}

// A list stored inside a map, then a deep snapshot round trip into a fresh doc.
static void test_nested_list_in_map_snapshot() {
    std::vector<std::uint8_t> snapshot;
    {
        loro::Doc doc;
        loro::Map m = doc.get_map("m");
        loro::Container attached = m.insert_container("items", loro::Container::list());
        loro::List items = attached.as_list();
        items.push("\"a\"");
        items.push("\"b\"");
        m.insert("count", "2");
        doc.commit();
        snapshot = doc.export_snapshot();
    }
    CHECK(!snapshot.empty());

    loro::Doc fresh;
    fresh.import(snapshot);
    loro::Map m = fresh.get_map("m");
    CHECK(m.get("count") == "2");

    std::optional<loro::Container> items = m.get_container("items");
    CHECK(items.has_value());
    loro::List l = items->as_list();
    CHECK(l.size() == 2);
    CHECK(l.get(0) == "\"a\"");
    CHECK(l.get(1) == "\"b\"");
}

// Free the Doc while a child Map is still held; the Map keeps the document state alive.
static void test_free_ordering() {
    auto doc = std::make_unique<loro::Doc>();
    loro::Map m = doc->get_map("m");
    m.insert("k", "1");
    doc->commit();

    doc.reset();  // free the LoroDoc while `m` is still alive

    CHECK(m.get("k") == "1");
    m.insert("k2", "2");  // still mutable
    CHECK(m.size() == 2);
}

int main() {
    test_map_basics();
    test_list_basics();
    test_nested_map_in_list();
    test_nested_list_in_map_snapshot();
    test_free_ordering();

    if (failures == 0) {
        std::puts("test_map_list: OK");
        return 0;
    }
    std::fprintf(stderr, "test_map_list: %d failure(s)\n", failures);
    return 1;
}
