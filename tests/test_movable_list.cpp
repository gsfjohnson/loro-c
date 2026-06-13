// M2: LoroMovableList — the two operations a plain list lacks: in-place `set` and element
// `move`, plus a snapshot round trip.

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

static void test_set() {
    loro::Doc doc;
    loro::MovableList l = doc.get_movable_list("l");
    l.push("\"a\"");
    l.push("\"b\"");
    l.push("\"c\"");
    doc.commit();
    CHECK(l.size() == 3);

    l.set(1, "\"B\"");  // replace in place
    doc.commit();
    CHECK(l.size() == 3);  // set does not change length
    CHECK(l.get(0) == "\"a\"");
    CHECK(l.get(1) == "\"B\"");
    CHECK(l.get(2) == "\"c\"");
}

static void test_move() {
    loro::Doc doc;
    loro::MovableList l = doc.get_movable_list("l");
    l.push("\"a\"");
    l.push("\"b\"");
    l.push("\"c\"");
    doc.commit();

    l.move(0, 2);  // move "a" to index 2 -> [b, c, a]
    doc.commit();
    CHECK(l.get(0) == "\"b\"");
    CHECK(l.get(1) == "\"c\"");
    CHECK(l.get(2) == "\"a\"");
}

static void test_nested_and_snapshot() {
    std::vector<std::uint8_t> snapshot;
    {
        loro::Doc doc;
        loro::MovableList l = doc.get_movable_list("l");
        loro::Container attached = l.push_container(loro::Container::map());
        loro::Map m = attached.as_map();
        m.insert("k", "\"v\"");
        l.push("7");
        doc.commit();
        snapshot = doc.export_snapshot();
    }
    CHECK(!snapshot.empty());

    loro::Doc fresh;
    fresh.import(snapshot);
    loro::MovableList l = fresh.get_movable_list("l");
    CHECK(l.size() == 2);
    std::optional<loro::Container> c = l.get_container(0);
    CHECK(c.has_value());
    CHECK(c->as_map().get("k") == "\"v\"");
    CHECK(l.get(1) == "7");
}

int main() {
    test_set();
    test_move();
    test_nested_and_snapshot();

    if (failures == 0) {
        std::puts("test_movable_list: OK");
        return 0;
    }
    std::fprintf(stderr, "test_movable_list: %d failure(s)\n", failures);
    return 1;
}
