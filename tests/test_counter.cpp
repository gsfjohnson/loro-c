// M2: LoroCounter — additive increment/decrement and a two-peer merge (the CRDT counter
// property: concurrent increments sum).

#include <loro/loro.hpp>

#include <cstdio>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

static bool approx(double a, double b) { return (a - b) < 1e-9 && (b - a) < 1e-9; }

static void test_basic() {
    loro::Doc doc;
    loro::Counter c = doc.get_counter("c");
    CHECK(approx(c.value(), 0.0));

    c.increment(5.0);
    c.increment(2.5);
    doc.commit();
    CHECK(approx(c.value(), 7.5));

    c.decrement(3.5);
    doc.commit();
    CHECK(approx(c.value(), 4.0));
}

// Two peers increment the same counter concurrently; after exchanging updates both
// converge to the sum.
static void test_merge() {
    loro::Doc alice;
    alice.set_peer_id(1);
    loro::Doc bob;
    bob.set_peer_id(2);

    loro::Counter ac = alice.get_counter("c");
    ac.increment(10.0);
    alice.commit();

    loro::Counter bc = bob.get_counter("c");
    bc.increment(5.0);
    bob.commit();

    alice.import(bob.export_updates());
    bob.import(alice.export_updates());

    CHECK(approx(alice.get_counter("c").value(), 15.0));
    CHECK(approx(bob.get_counter("c").value(), 15.0));
}

int main() {
    test_basic();
    test_merge();

    if (failures == 0) {
        std::puts("test_counter: OK");
        return 0;
    }
    std::fprintf(stderr, "test_counter: %d failure(s)\n", failures);
    return 1;
}
