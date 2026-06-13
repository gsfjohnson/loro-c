// M1: LoroDoc lifecycle, peer id, fork, and whole-document JSON via the C++ RAII wrapper.

#include <loro/loro.hpp>

#include <cstdio>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__,      \
                         __LINE__, #cond);                                     \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int main() {
    // Version string is non-empty.
    CHECK(!loro::version().empty());

    // New doc, set/get peer id.
    loro::Doc doc;
    doc.set_peer_id(42);
    CHECK(doc.peer_id() == 42);

    // Populate and commit.
    {
        loro::Text t = doc.get_text("greeting");
        t.insert(0, "hello");
        doc.commit();
    }

    // Whole-document JSON reflects the text.
    const std::string json = doc.to_json();
    CHECK(json.find("hello") != std::string::npos);
    CHECK(json.find("greeting") != std::string::npos);

    // Fork shares the state at fork time.
    loro::Doc forked = doc.fork();
    CHECK(forked.to_json().find("hello") != std::string::npos);

    // A divergent edit on the fork does not affect the original.
    {
        loro::Text t = forked.get_text("greeting");
        t.insert(t.len_unicode(), " world");
        forked.commit();
    }
    CHECK(forked.get_text("greeting").to_string() == "hello world");
    CHECK(doc.get_text("greeting").to_string() == "hello");

    if (failures == 0) {
        std::puts("test_doc: OK");
        return 0;
    }
    std::fprintf(stderr, "test_doc: %d failure(s)\n", failures);
    return 1;
}
