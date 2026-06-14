/* M1: proves the C ABI is usable from plain C (this file is compiled as C, never C++),
 * covering the snapshot round trip and the free-the-doc-before-the-text ordering case. */

#include <loro/loro.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #cond);                                                    \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

/* Compares a (non-nul-terminated) LoroBytes buffer to a C string literal. */
static int bytes_eq(const LoroBytes* b, const char* s) {
    size_t n = strlen(s);
    return b->len == n && (n == 0 || memcmp(b->data, s, n) == 0);
}

static void test_snapshot_round_trip(void) {
    LoroDoc* doc = loro_doc_new();
    CHECK(doc != NULL);

    LoroText* t = loro_doc_get_text(doc, "t", 1);
    CHECK(t != NULL);
    CHECK(loro_text_insert(t, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_text_len_unicode(t) == 5);

    LoroBytes snapshot = {0};
    CHECK(loro_doc_export_snapshot(doc, &snapshot) == LORO_OK);
    CHECK(snapshot.len > 0);

    loro_text_free(t);
    loro_doc_free(doc);

    /* Import into a fresh doc and read it back. */
    LoroDoc* fresh = loro_doc_new();
    CHECK(loro_doc_import(fresh, snapshot.data, snapshot.len) == LORO_OK);
    loro_bytes_free(snapshot);

    LoroText* t2 = loro_doc_get_text(fresh, "t", 1);
    LoroBytes str = {0};
    CHECK(loro_text_to_string(t2, &str) == LORO_OK);
    CHECK(bytes_eq(&str, "hello"));
    loro_bytes_free(str);

    loro_text_free(t2);
    loro_doc_free(fresh);
}

/* Free the LoroDoc* BEFORE a still-held LoroText*, then use and free the text handle.
 * Confirms the strong-co-ownership model: no use-after-free, no leak. */
static void test_free_ordering(void) {
    LoroDoc* doc = loro_doc_new();
    LoroText* t = loro_doc_get_text(doc, "t", 1);
    CHECK(loro_text_insert(t, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    loro_doc_free(doc); /* free the doc first */

    /* t is still valid: it co-owns the document state. */
    LoroBytes str = {0};
    CHECK(loro_text_to_string(t, &str) == LORO_OK);
    CHECK(bytes_eq(&str, "hello"));
    loro_bytes_free(str);
    CHECK(loro_text_len_unicode(t) == 5);

    loro_text_free(t); /* releases the last reference */
}

/* Error path: inserting invalid UTF-8 returns LORO_ERR_UTF8 and records a message;
 * a null handle returns LORO_ERR_INVALID_ARG. */
static void test_error_path(void) {
    LoroDoc* doc = loro_doc_new();
    LoroText* t = loro_doc_get_text(doc, "t", 1);

    const char bad[] = {(char)0xFF, (char)0xFE}; /* not valid UTF-8 */
    CHECK(loro_text_insert(t, 0, bad, sizeof bad) == LORO_ERR_UTF8);
    CHECK(loro_last_error_message() != NULL);

    CHECK(loro_doc_commit(NULL) == LORO_ERR_INVALID_ARG);

    loro_text_free(t);
    loro_doc_free(doc);
}

/* M2 containers from plain C: map insert/get, list push/get, and a nested map inside a
 * list attached and read back through the type-erased LoroContainer. */
static void test_containers(void) {
    LoroDoc* doc = loro_doc_new();

    /* Map: insert a couple of JSON values and read one back. */
    LoroMap* m = loro_doc_get_map(doc, "m", 1);
    CHECK(m != NULL);
    CHECK(loro_map_insert(m, "n", 1, "42", 2) == LORO_OK);
    CHECK(loro_map_insert(m, "s", 1, "\"hi\"", 4) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_map_len(m) == 2);

    LoroBytes nval = {0};
    CHECK(loro_map_get(m, "n", 1, &nval) == LORO_OK);
    CHECK(bytes_eq(&nval, "42"));
    loro_bytes_free(nval);

    /* A missing key reports NOT_FOUND. */
    LoroBytes missing = {0};
    CHECK(loro_map_get(m, "x", 1, &missing) == LORO_ERR_NOT_FOUND);

    /* List: push two values, read one back. */
    LoroList* l = loro_doc_get_list(doc, "l", 1);
    CHECK(loro_list_push(l, "1", 1) == LORO_OK);
    CHECK(loro_list_push(l, "2", 1) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_list_len(l) == 2);

    LoroBytes first = {0};
    CHECK(loro_list_get(l, 0, &first) == LORO_OK);
    CHECK(bytes_eq(&first, "1"));
    loro_bytes_free(first);

    /* Nested: attach a detached map into the list, write a key, read it back. */
    LoroContainer* detached = loro_container_new(LORO_CONTAINER_MAP);
    CHECK(detached != NULL);
    LoroContainer* attached = loro_list_push_container(l, detached);
    CHECK(attached != NULL);
    CHECK(loro_container_type(attached) == LORO_CONTAINER_MAP);
    LoroMap* child = loro_container_get_map(attached);
    CHECK(child != NULL);
    CHECK(loro_map_insert(child, "k", 1, "true", 4) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    LoroContainer* got = loro_list_get_container(l, 2);
    CHECK(got != NULL);
    LoroMap* child2 = loro_container_get_map(got);
    LoroBytes kval = {0};
    CHECK(loro_map_get(child2, "k", 1, &kval) == LORO_OK);
    CHECK(bytes_eq(&kval, "true"));
    loro_bytes_free(kval);

    loro_map_free(child2);
    loro_container_free(got);
    loro_map_free(child);
    loro_container_free(attached);
    loro_list_free(l);
    loro_map_free(m);
    loro_doc_free(doc);
}

/* M3: subscribe to the whole document, walk the DiffEvent envelope + JSON payload, and
 * confirm unsubscribe (free) stops callbacks and runs free_user_data exactly once. */
typedef struct {
    int calls;
    int last_trigger;
    size_t last_count;
    char target[64];
    char json[256];
} sub_capture;

static int g_freed = 0;

static void copy_bytes(char* dst, size_t dst_cap, const LoroBytes* b) {
    size_t n = b->len < dst_cap - 1 ? b->len : dst_cap - 1;
    if (n > 0) memcpy(dst, b->data, n);
    dst[n] = '\0';
}

static void on_event(const LoroDiffEvent* ev, void* ud) {
    sub_capture* cap = (sub_capture*)ud;
    ++cap->calls;
    cap->last_trigger = (int)loro_diff_event_triggered_by(ev);
    cap->last_count = loro_diff_event_count(ev);
    if (loro_diff_event_count(ev) > 0) {
        const LoroContainerDiff* d = loro_diff_event_get(ev, 0);
        LoroBytes tb = {0};
        if (loro_container_diff_target(d, &tb) == LORO_OK) {
            copy_bytes(cap->target, sizeof cap->target, &tb);
            loro_bytes_free(tb);
        }
        LoroBytes jb = {0};
        if (loro_container_diff_to_json(d, &jb) == LORO_OK) {
            copy_bytes(cap->json, sizeof cap->json, &jb);
            loro_bytes_free(jb);
        }
    }
}

static void on_free(void* ud) {
    (void)ud;
    ++g_freed;
}

static void test_subscribe(void) {
    LoroDoc* doc = loro_doc_new();
    LoroText* t = loro_doc_get_text(doc, "t", 1);

    sub_capture cap;
    memset(&cap, 0, sizeof cap);

    LoroSubscriber cb;
    cb.invoke = on_event;
    cb.user_data = &cap;
    cb.free_user_data = on_free;

    LoroSubscription* sub = loro_doc_subscribe_root(doc, cb);
    CHECK(sub != NULL);

    CHECK(loro_text_insert(t, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    CHECK(cap.calls == 1);
    CHECK(cap.last_trigger == LORO_EVENT_TRIGGER_LOCAL);
    CHECK(cap.last_count >= 1);
    CHECK(strstr(cap.json, "hello") != NULL);

    /* The diff target matches the text container's id string. */
    LoroBytes id = {0};
    CHECK(loro_text_id(t, &id) == LORO_OK);
    CHECK(bytes_eq(&id, cap.target));
    loro_bytes_free(id);

    /* Freeing the subscription unsubscribes and runs free_user_data exactly once. */
    loro_subscription_free(sub);
    CHECK(g_freed == 1);

    CHECK(loro_text_insert(t, 5, "!", 1) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(cap.calls == 1); /* no further callbacks after unsubscribe */

    loro_text_free(t);
    loro_doc_free(doc);
}

/* M4: a slice of the advanced surface from plain C — version-vector delta sync and a
 * fractional index between two others. */
static void test_advanced_c(void) {
    LoroDoc* a = loro_doc_new();
    LoroText* ta = loro_doc_get_text(a, "t", 1);
    CHECK(loro_text_insert(ta, 0, "hi", 2) == LORO_OK);
    CHECK(loro_doc_commit(a) == LORO_OK);

    /* Delta from an empty doc's vv brings a fresh doc fully up to date. */
    LoroDoc* b = loro_doc_new();
    LoroVersionVector* b_vv = loro_doc_state_vv(b);
    CHECK(b_vv != NULL);
    LoroBytes delta = {0};
    CHECK(loro_doc_export_updates_from(a, b_vv, &delta) == LORO_OK);
    CHECK(delta.len > 0);
    CHECK(loro_doc_import(b, delta.data, delta.len) == LORO_OK);
    loro_bytes_free(delta);

    LoroText* tb = loro_doc_get_text(b, "t", 1);
    LoroBytes str = {0};
    CHECK(loro_text_to_string(tb, &str) == LORO_OK);
    CHECK(bytes_eq(&str, "hi"));
    loro_bytes_free(str);

    /* a includes everything b has. */
    LoroVersionVector* a_vv = loro_doc_oplog_vv(a);
    LoroVersionVector* b_oplog = loro_doc_oplog_vv(b);
    CHECK(loro_version_vector_includes_vv(a_vv, b_oplog));

    /* version-vector encode/decode round trip. */
    LoroBytes enc = {0};
    CHECK(loro_version_vector_encode(a_vv, &enc) == LORO_OK);
    LoroVersionVector* dec = loro_version_vector_decode(enc.data, enc.len);
    CHECK(dec != NULL);
    int32_t ord = 99;
    CHECK(loro_version_vector_compare(a_vv, dec, &ord) == LORO_OK);
    CHECK(ord == 0);
    loro_bytes_free(enc);

    loro_version_vector_free(dec);
    loro_version_vector_free(b_oplog);
    loro_version_vector_free(a_vv);
    loro_version_vector_free(b_vv);
    loro_text_free(tb);
    loro_doc_free(b);
    loro_text_free(ta);
    loro_doc_free(a);

    /* Fractional index: default < between(default, NULL). */
    LoroFractionalIndex* lo = loro_fractional_index_default();
    LoroFractionalIndex* hi = loro_fractional_index_between(lo, NULL);
    CHECK(hi != NULL);
    int32_t c = 0;
    CHECK(loro_fractional_index_compare(hi, lo, &c) == LORO_OK);
    CHECK(c > 0);
    loro_fractional_index_free(hi);
    loro_fractional_index_free(lo);
}

int main(void) {
    CHECK(loro_version() != NULL);
    test_snapshot_round_trip();
    test_free_ordering();
    test_error_path();
    test_containers();
    test_subscribe();
    test_advanced_c();

    if (failures == 0) {
        puts("test_c_only: OK");
        return 0;
    }
    fprintf(stderr, "test_c_only: %d failure(s)\n", failures);
    return 1;
}
