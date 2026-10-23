/* M1: proves the C ABI is usable from plain C (this file is compiled as C, never C++),
 * covering the snapshot round trip and the free-the-doc-before-the-text ordering case. */

#include <loro/loro.h>

#include <stdio.h>
#include <stdlib.h>
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

/* Whether the (non-nul-terminated) LoroBytes buffer contains `s` as a substring. */
static int bytes_contains(const LoroBytes* b, const char* s) {
    size_t n = strlen(s);
    if (n == 0) return 1;
    if (b->len < n) return 0;
    for (size_t i = 0; i + n <= b->len; ++i) {
        if (memcmp(b->data + i, s, n) == 0) return 1;
    }
    return 0;
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

/* G1: rich text from plain C — mark/unmark, richtext-value JSON, splice, push_str, and the
 * style-config builder + doc config. */
static void test_richtext_c(void) {
    LoroDoc* doc = loro_doc_new();
    LoroText* t = loro_doc_get_text(doc, "t", 1);
    CHECK(loro_text_insert(t, 0, "Hello world!", 12) == LORO_OK);

    /* "bold" is in the default rich-text config, so this marks without extra setup. */
    CHECK(loro_text_mark(t, 0, 5, "bold", 4, "true", 4) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    LoroBytes rv = {0};
    CHECK(loro_text_get_richtext_value(t, &rv) == LORO_OK);
    CHECK(bytes_contains(&rv, "bold"));
    CHECK(bytes_contains(&rv, "Hello"));
    loro_bytes_free(rv);

    LoroBytes delta = {0};
    CHECK(loro_text_to_delta(t, &delta) == LORO_OK);
    CHECK(bytes_contains(&delta, "bold"));
    loro_bytes_free(delta);

    CHECK(loro_text_unmark(t, 0, 5, "bold", 4) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    /* splice returns the removed slice: "Hello world!" -> "world!". */
    LoroBytes removed = {0};
    CHECK(loro_text_splice(t, 0, 6, "", 0, &removed) == LORO_OK);
    CHECK(bytes_eq(&removed, "Hello "));
    loro_bytes_free(removed);

    CHECK(loro_text_push_str(t, "X", 1) == LORO_OK);
    LoroBytes str = {0};
    CHECK(loro_text_to_string(t, &str) == LORO_OK);
    CHECK(bytes_eq(&str, "world!X"));
    loro_bytes_free(str);

    loro_text_free(t);
    loro_doc_free(doc);

    /* style-config builder + doc config, then mark with the custom key. */
    LoroDoc* d2 = loro_doc_new();
    LoroStyleConfigMap* styles = loro_style_config_map_new();
    CHECK(styles != NULL);
    LoroStyleConfig cfg;
    cfg.expand = LORO_EXPAND_NONE;
    CHECK(loro_style_config_map_insert(styles, "fmt", 3, cfg) == LORO_OK);
    CHECK(loro_doc_config_text_style(d2, styles) == LORO_OK);
    loro_style_config_map_free(styles);

    LoroText* t2 = loro_doc_get_text(d2, "t", 1);
    CHECK(loro_text_insert(t2, 0, "abc", 3) == LORO_OK);
    CHECK(loro_text_mark(t2, 0, 3, "fmt", 3, "true", 4) == LORO_OK);
    CHECK(loro_doc_commit(d2) == LORO_OK);
    loro_text_free(t2);
    loro_doc_free(d2);
}

/* G2: cursors from plain C — create on text, resolve to an absolute position, watch it shift
 * after a front insert, encode/decode round-trip, and the configured Side survives deletion. */
static void test_cursor_c(void) {
    LoroDoc* doc = loro_doc_new();
    LoroText* t = loro_doc_get_text(doc, "t", 1);
    CHECK(loro_text_insert(t, 0, "Hello world", 11) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    /* Cursor anchored at codepoint 3 (default-ish: explicit Middle). */
    LoroCursor* cur = loro_text_get_cursor(t, 3, LORO_SIDE_MIDDLE);
    CHECK(cur != NULL);

    LoroPosQueryResult r = {0};
    CHECK(loro_doc_get_cursor_pos(doc, cur, &r) == LORO_OK);
    CHECK(r.abs_pos == 3);

    /* Insert before the cursor: the absolute position shifts right by 3. */
    CHECK(loro_text_insert(t, 0, "XYZ", 3) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_doc_get_cursor_pos(doc, cur, &r) == LORO_OK);
    CHECK(r.abs_pos == 6);

    /* encode -> decode reproduces an equivalent cursor. */
    LoroBytes enc = {0};
    CHECK(loro_cursor_encode(cur, &enc) == LORO_OK);
    CHECK(enc.len > 0);
    LoroCursor* decoded = loro_cursor_decode(enc.data, enc.len);
    CHECK(decoded != NULL);
    loro_bytes_free(enc);
    LoroPosQueryResult r2 = {0};
    CHECK(loro_doc_get_cursor_pos(doc, decoded, &r2) == LORO_OK);
    CHECK(r2.abs_pos == 6);

    loro_cursor_free(decoded);
    loro_cursor_free(cur);
    loro_text_free(t);
    loro_doc_free(doc);

    /* Configured Side (Left) is reported and survives deletion of the anchor. */
    LoroDoc* d2 = loro_doc_new();
    LoroText* t2 = loro_doc_get_text(d2, "t", 1);
    CHECK(loro_text_insert(t2, 0, "Hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(d2) == LORO_OK);
    LoroCursor* lc = loro_text_get_cursor(t2, 2, LORO_SIDE_LEFT);
    CHECK(lc != NULL);
    LoroPosQueryResult lr = {0};
    CHECK(loro_doc_get_cursor_pos(d2, lc, &lr) == LORO_OK);
    CHECK(lr.abs_pos == 2);
    CHECK(lr.side == LORO_SIDE_LEFT);

    CHECK(loro_text_delete(t2, 2, 1) == LORO_OK); /* delete the anchored char */
    CHECK(loro_doc_commit(d2) == LORO_OK);
    CHECK(loro_doc_get_cursor_pos(d2, lc, &lr) == LORO_OK);
    CHECK(lr.abs_pos == 2);
    CHECK(lr.side == LORO_SIDE_LEFT);

    loro_cursor_free(lc);
    loro_text_free(t2);
    loro_doc_free(d2);
}

/* G3: JSON-update sync + export modes from plain C — round-trip JSON updates between two docs,
 * an id-span JSON export, import_batch of two blobs, and shallow-snapshot introspection. */
static void test_json_sync_c(void) {
    LoroDoc* a = loro_doc_new();
    CHECK(loro_doc_set_peer_id(a, 1) == LORO_OK);
    LoroText* ta = loro_doc_get_text(a, "t", 1);
    CHECK(loro_text_insert(ta, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(a) == LORO_OK);

    /* export_json_updates(empty .. oplog_vv) -> JSON carrying the schema and the text. */
    LoroVersionVector* empty = loro_version_vector_new();
    LoroVersionVector* a_vv = loro_doc_oplog_vv(a);
    LoroBytes json = {0};
    CHECK(loro_doc_export_json_updates(a, empty, a_vv, &json) == LORO_OK);
    CHECK(json.len > 0);
    CHECK(bytes_contains(&json, "schema_version"));
    CHECK(bytes_contains(&json, "hello"));

    /* import_json_updates into a fresh doc reproduces the text. */
    LoroDoc* b = loro_doc_new();
    CHECK(loro_doc_import_json_updates(b, (const char*)json.data, json.len) == LORO_OK);
    LoroText* tb = loro_doc_get_text(b, "t", 1);
    LoroBytes sb = {0};
    CHECK(loro_text_to_string(tb, &sb) == LORO_OK);
    CHECK(bytes_eq(&sb, "hello"));
    loro_bytes_free(sb);
    loro_bytes_free(json);

    /* export_json_in_id_span over peer 1's [0,5) returns a non-empty JSON array. */
    LoroIdSpan span = {1, 0, 5};
    LoroBytes span_json = {0};
    CHECK(loro_doc_export_json_in_id_span(a, span, &span_json) == LORO_OK);
    CHECK(span_json.len > 0);
    CHECK(bytes_contains(&span_json, "hello"));
    loro_bytes_free(span_json);

    loro_version_vector_free(empty);
    loro_version_vector_free(a_vv);
    loro_text_free(tb);
    loro_text_free(ta);
    loro_doc_free(b);
    loro_doc_free(a);

    /* import_batch: two independent docs' updates merged into a third in one call. */
    LoroDoc* p = loro_doc_new();
    CHECK(loro_doc_set_peer_id(p, 11) == LORO_OK);
    LoroText* tp = loro_doc_get_text(p, "p", 1);
    CHECK(loro_text_insert(tp, 0, "P", 1) == LORO_OK);
    CHECK(loro_doc_commit(p) == LORO_OK);
    LoroBytes up = {0};
    CHECK(loro_doc_export_updates(p, &up) == LORO_OK);

    LoroDoc* q = loro_doc_new();
    CHECK(loro_doc_set_peer_id(q, 22) == LORO_OK);
    LoroText* tq = loro_doc_get_text(q, "q", 1);
    CHECK(loro_text_insert(tq, 0, "Q", 1) == LORO_OK);
    CHECK(loro_doc_commit(q) == LORO_OK);
    LoroBytes uq = {0};
    CHECK(loro_doc_export_updates(q, &uq) == LORO_OK);

    LoroDoc* r = loro_doc_new();
    const uint8_t* datas[2] = {up.data, uq.data};
    size_t lens[2] = {up.len, uq.len};
    CHECK(loro_doc_import_batch(r, datas, lens, 2) == LORO_OK);
    LoroText* rp = loro_doc_get_text(r, "p", 1);
    LoroText* rq = loro_doc_get_text(r, "q", 1);
    LoroBytes rps = {0};
    LoroBytes rqs = {0};
    CHECK(loro_text_to_string(rp, &rps) == LORO_OK);
    CHECK(bytes_eq(&rps, "P"));
    CHECK(loro_text_to_string(rq, &rqs) == LORO_OK);
    CHECK(bytes_eq(&rqs, "Q"));
    loro_bytes_free(rps);
    loro_bytes_free(rqs);
    loro_bytes_free(up);
    loro_bytes_free(uq);
    loro_text_free(rp);
    loro_text_free(rq);
    loro_text_free(tp);
    loro_text_free(tq);
    loro_doc_free(r);
    loro_doc_free(p);
    loro_doc_free(q);

    /* Shallow snapshot: export from the frontiers after the first of two commits, import into a
     * fresh doc, and confirm the result is shallow with a non-empty shallow_since_vv. */
    LoroDoc* c = loro_doc_new();
    CHECK(loro_doc_set_peer_id(c, 7) == LORO_OK);
    LoroText* tc = loro_doc_get_text(c, "t", 1);
    CHECK(loro_text_insert(tc, 0, "AAA", 3) == LORO_OK);
    CHECK(loro_doc_commit(c) == LORO_OK);
    LoroFrontiers* mid = loro_doc_state_frontiers(c);
    CHECK(loro_text_insert(tc, 3, "BBB", 3) == LORO_OK);
    CHECK(loro_doc_commit(c) == LORO_OK);

    LoroBytes shallow = {0};
    CHECK(loro_doc_export_shallow_snapshot(c, mid, &shallow) == LORO_OK);
    CHECK(shallow.len > 0);
    loro_frontiers_free(mid);

    LoroDoc* d = loro_doc_new();
    CHECK(loro_doc_import(d, shallow.data, shallow.len) == LORO_OK);
    loro_bytes_free(shallow);
    CHECK(loro_doc_is_shallow(d) == true);
    CHECK(loro_doc_is_shallow(c) == false);
    LoroVersionVector* since = loro_doc_shallow_since_vv(d);
    CHECK(since != NULL);
    LoroBytes since_json = {0};
    CHECK(loro_version_vector_to_json(since, &since_json) == LORO_OK);
    CHECK(since_json.len > 2); /* not the empty "{}" */
    loro_bytes_free(since_json);
    /* the retained state is still "AAABBB". */
    LoroText* td = loro_doc_get_text(d, "t", 1);
    LoroBytes ds = {0};
    CHECK(loro_text_to_string(td, &ds) == LORO_OK);
    CHECK(bytes_eq(&ds, "AAABBB"));
    loro_bytes_free(ds);

    loro_version_vector_free(since);
    loro_text_free(td);
    loro_text_free(tc);
    loro_doc_free(d);
    loro_doc_free(c);
}

/* G4: diff between two versions, apply a diff onto another doc, and revert. */
static void test_diff_c(void) {
    LoroDoc* a = loro_doc_new();
    CHECK(loro_doc_set_peer_id(a, 1) == LORO_OK);
    LoroText* ta = loro_doc_get_text(a, "t", 1);
    CHECK(loro_text_insert(ta, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(a) == LORO_OK);
    LoroFrontiers* f0 = loro_doc_state_frontiers(a);
    CHECK(f0 != NULL);

    CHECK(loro_text_insert(ta, 5, " world", 6) == LORO_OK);
    CHECK(loro_doc_commit(a) == LORO_OK);
    LoroFrontiers* f1 = loro_doc_state_frontiers(a);
    CHECK(f1 != NULL);

    /* diff(F0, F1) describes inserting " world"; the JSON view reflects that. */
    LoroDiffBatch* batch = NULL;
    CHECK(loro_doc_diff(a, f0, f1, &batch) == LORO_OK);
    CHECK(batch != NULL);
    LoroBytes dj = {0};
    CHECK(loro_diff_batch_to_json(batch, &dj) == LORO_OK);
    CHECK(bytes_contains(&dj, "insert"));
    CHECK(bytes_contains(&dj, "world"));
    loro_bytes_free(dj);

    /* Apply the batch onto a clone frozen at F0: it reaches A's F1 state. */
    LoroBytes snap0 = {0};
    CHECK(loro_doc_export_snapshot_at(a, f0, &snap0) == LORO_OK);
    LoroDoc* b = loro_doc_new();
    CHECK(loro_doc_import(b, snap0.data, snap0.len) == LORO_OK);
    loro_bytes_free(snap0);
    LoroText* tb = loro_doc_get_text(b, "t", 1);
    LoroBytes bs0 = {0};
    CHECK(loro_text_to_string(tb, &bs0) == LORO_OK);
    CHECK(bytes_eq(&bs0, "hello"));
    loro_bytes_free(bs0);
    CHECK(loro_doc_apply_diff(b, batch) == LORO_OK);
    LoroBytes bs1 = {0};
    CHECK(loro_text_to_string(tb, &bs1) == LORO_OK);
    CHECK(bytes_eq(&bs1, "hello world"));
    loro_bytes_free(bs1);

    /* Batch is not consumed by apply_diff; the caller still frees it. */
    loro_diff_batch_free(batch);

    /* revert_to rewinds A to the F0 content (recording the inverse as new ops). */
    CHECK(loro_doc_revert_to(a, f0) == LORO_OK);
    LoroBytes as = {0};
    CHECK(loro_text_to_string(ta, &as) == LORO_OK);
    CHECK(bytes_eq(&as, "hello"));
    loro_bytes_free(as);

    /* Error path: a null out-param is rejected. */
    CHECK(loro_doc_diff(a, f0, f1, NULL) == LORO_ERR_INVALID_ARG);

    loro_frontiers_free(f0);
    loro_frontiers_free(f1);
    loro_text_free(tb);
    loro_text_free(ta);
    loro_doc_free(b);
    loro_doc_free(a);
}

/* G5: navigate to a value or live container by path. The key assertion is that a container
 * obtained via get_by_* is LIVE — mutating it shows up on the parent document, which a flat
 * JSON dump could never round-trip. */
static void test_get_by_path_c(void) {
    LoroDoc* doc = loro_doc_new();
    CHECK(doc != NULL);

    /* items[0] = { "name": "alice" } */
    LoroList* items = loro_doc_get_list(doc, "items", 5);
    CHECK(items != NULL);
    LoroContainer* detached = loro_container_new(LORO_CONTAINER_MAP);
    LoroContainer* attached = loro_list_push_container(items, detached); /* consumes detached */
    CHECK(attached != NULL);
    LoroMap* m0 = loro_container_get_map(attached);
    CHECK(m0 != NULL);
    CHECK(loro_map_insert(m0, "name", 4, "\"alice\"", 7) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    /* Navigate by string path to the nested container. */
    LoroValueOrContainer* voc = loro_doc_get_by_str_path(doc, "items/0", 7);
    CHECK(voc != NULL);
    CHECK(loro_value_or_container_is_container(voc) == true);
    CHECK(loro_value_or_container_container_type(voc) == LORO_CONTAINER_MAP);

    /* The handle is LIVE: write a new key through it, then observe it on the parent doc. */
    LoroContainer* live = loro_value_or_container_get_container(voc);
    CHECK(live != NULL);
    LoroMap* live_map = loro_container_get_map(live);
    CHECK(live_map != NULL);
    CHECK(loro_map_insert(live_map, "age", 3, "30", 2) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    LoroBytes dj = {0};
    CHECK(loro_doc_get_deep_value_json(doc, &dj) == LORO_OK);
    CHECK(bytes_contains(&dj, "age"));
    CHECK(bytes_contains(&dj, "30"));
    loro_bytes_free(dj);

    /* Navigate to a leaf value: not a container, read as JSON. */
    LoroValueOrContainer* leaf = loro_doc_get_by_str_path(doc, "items/0/name", 12);
    CHECK(leaf != NULL);
    CHECK(loro_value_or_container_is_container(leaf) == false);
    CHECK(loro_value_or_container_container_type(leaf) == LORO_CONTAINER_UNKNOWN);
    CHECK(loro_value_or_container_get_container(leaf) == NULL); /* it's a value */
    LoroBytes lj = {0};
    CHECK(loro_value_or_container_get_value_json(leaf, &lj) == LORO_OK);
    CHECK(bytes_contains(&lj, "alice"));
    loro_bytes_free(lj);

    /* The same node via explicit {KEY, SEQ} path components. */
    LoroPathComponent path[2];
    memset(path, 0, sizeof(path));
    path[0].kind = LORO_PATH_KEY;
    path[0].key = "items";
    path[0].key_len = 5;
    path[1].kind = LORO_PATH_SEQ;
    path[1].seq = 0;
    LoroValueOrContainer* voc2 = loro_doc_get_by_path(doc, path, 2);
    CHECK(voc2 != NULL);
    CHECK(loro_value_or_container_is_container(voc2) == true);
    CHECK(loro_value_or_container_container_type(voc2) == LORO_CONTAINER_MAP);

    /* A path that does not resolve returns NULL. */
    CHECK(loro_doc_get_by_str_path(doc, "items/99", 8) == NULL);

    loro_value_or_container_free(voc2);
    loro_value_or_container_free(leaf);
    loro_value_or_container_free(voc);
    loro_map_free(live_map);
    loro_container_free(live);
    loro_map_free(m0);
    loro_container_free(attached);
    loro_list_free(items);
    loro_doc_free(doc);
}

/* G6.1: doc config & timestamps via the raw C surface. The Configure handle shares the
   document's live config, so a change through it (or via the doc shortcuts) is visible
   through a freshly fetched handle. */
static void test_g6_c(void) {
    LoroDoc* doc = loro_doc_new();
    CHECK(doc != NULL);

    /* Upstream defaults: timestamps off, merge interval 1000s. */
    LoroConfigure* cfg = loro_doc_config(doc);
    CHECK(cfg != NULL);
    CHECK(loro_configure_record_timestamp(cfg) == false);
    CHECK(loro_configure_merge_interval(cfg) == 1000);

    /* A change through one handle is visible through a separately fetched handle. */
    loro_configure_set_record_timestamp(cfg, true);
    loro_configure_set_merge_interval(cfg, 42);
    LoroConfigure* cfg2 = loro_doc_config(doc);
    CHECK(cfg2 != NULL);
    CHECK(loro_configure_record_timestamp(cfg2) == true);
    CHECK(loro_configure_merge_interval(cfg2) == 42);
    loro_configure_free(cfg2);

    /* The doc-level shortcuts agree with the Configure getters. */
    loro_doc_set_record_timestamp(doc, false);
    loro_doc_set_change_merge_interval(doc, 9000000000LL);
    LoroConfigure* cfg3 = loro_doc_config(doc);
    CHECK(cfg3 != NULL);
    CHECK(loro_configure_record_timestamp(cfg3) == false);
    CHECK(loro_configure_merge_interval(cfg3) == 9000000000LL);
    loro_configure_free(cfg3);

    /* Null-handle paths are no-ops / return the documented fallbacks. */
    CHECK(loro_doc_config(NULL) == NULL);
    CHECK(loro_configure_record_timestamp(NULL) == false);
    CHECK(loro_configure_merge_interval(NULL) == 0);
    loro_configure_free(NULL);
    loro_doc_set_record_timestamp(NULL, true);

    loro_configure_free(cfg);
    loro_doc_free(doc);

    /* --- G6.2: doc history & introspection --- */
    LoroDoc* d = loro_doc_new();
    CHECK(d != NULL);
    CHECK(loro_doc_set_peer_id(d, 5) == LORO_OK);

    CHECK(loro_doc_len_ops(d) == 0);
    CHECK(loro_doc_len_changes(d) == 0);

    LoroText* t = loro_doc_get_text(d, "t", 1);
    CHECK(loro_text_insert(t, 0, "hi", 2) == LORO_OK);
    CHECK(loro_doc_get_pending_txn_len(d) > 0);
    loro_doc_commit(d);
    CHECK(loro_doc_get_pending_txn_len(d) == 0);
    CHECK(loro_doc_len_ops(d) >= 1);
    CHECK(loro_doc_len_changes(d) == 1);

    /* has_container / try_get over the text's own cid, plus a bogus one. */
    LoroBytes tid = {0};
    CHECK(loro_text_id(t, &tid) == LORO_OK);
    CHECK(loro_doc_has_container(d, (const char*)tid.data, tid.len) == true);
    CHECK(loro_doc_has_container(d, "cid:99@99:Text", 14) == false);

    LoroText* got = loro_doc_try_get_text(d, (const char*)tid.data, tid.len);
    CHECK(got != NULL);
    loro_text_free(got);
    CHECK(loro_doc_try_get_text(d, "cid:99@99:Text", 14) == NULL);

    /* get_changed_containers_in lists the edited container's cid. */
    LoroId first = {5, 0};
    LoroBytes changed = {0};
    CHECK(loro_doc_get_changed_containers_in(d, first, 2, &changed) == LORO_OK);
    {
        char cidbuf[128];
        size_t n = tid.len < sizeof(cidbuf) - 1 ? tid.len : sizeof(cidbuf) - 1;
        memcpy(cidbuf, tid.data, n);
        cidbuf[n] = '\0';
        CHECK(bytes_contains(&changed, cidbuf) == 1);
    }
    loro_bytes_free(changed);
    loro_bytes_free(tid);

    /* get_change resolves the first change; bogus ids -> NOT_FOUND. */
    LoroChangeMetaOwned* meta = NULL;
    CHECK(loro_doc_get_change(d, first, &meta) == LORO_OK);
    CHECK(meta != NULL);
    {
        const LoroChangeMeta* ref = loro_change_meta_owned_as_ref(meta);
        LoroId mid = loro_change_meta_id(ref);
        CHECK(mid.peer == 5);
        CHECK(mid.counter == 0);
        CHECK(loro_change_meta_len(ref) >= 1);
    }
    loro_change_meta_owned_free(meta);

    LoroId bogus = {5, 99999};
    LoroChangeMetaOwned* nometa = NULL;
    CHECK(loro_doc_get_change(d, bogus, &nometa) == LORO_ERR_NOT_FOUND);
    CHECK(nometa == NULL);

    /* cmp_with_frontiers (equal to own version), minimize_frontiers, fork_at. */
    LoroFrontiers* f_now = loro_doc_state_frontiers(d);
    CHECK(f_now != NULL);
    int32_t ord = 7;
    CHECK(loro_doc_cmp_with_frontiers(d, f_now, &ord) == LORO_OK);
    CHECK(ord == 0);
    LoroFrontiers* fmin = loro_doc_minimize_frontiers(d, f_now);
    CHECK(fmin != NULL);
    loro_frontiers_free(fmin);

    LoroDoc* forked = loro_doc_fork_at(d, f_now);
    CHECK(forked != NULL);
    {
        LoroText* ft = loro_doc_get_text(forked, "t", 1);
        LoroBytes fs = {0};
        CHECK(loro_text_to_string(ft, &fs) == LORO_OK);
        CHECK(bytes_eq(&fs, "hi"));
        loro_bytes_free(fs);
        loro_text_free(ft);
    }
    loro_doc_free(forked);
    loro_frontiers_free(f_now);

    /* Cache controls + hide-empty-root all succeed. */
    CHECK(loro_doc_free_history_cache(d) == LORO_OK);
    CHECK(loro_doc_free_diff_calculator(d) == LORO_OK);
    CHECK(loro_doc_compact_change_store(d) == LORO_OK);
    CHECK(loro_doc_set_hide_empty_root_containers(d, true) == LORO_OK);

    /* Null-handle fallbacks for the new surface. */
    CHECK(loro_doc_len_ops(NULL) == 0);
    CHECK(loro_doc_has_history_cache(NULL) == false);
    CHECK(loro_doc_has_container(NULL, "x", 1) == false);
    CHECK(loro_doc_try_get_text(NULL, "x", 1) == NULL);
    loro_change_meta_owned_free(NULL);

    loro_text_free(t);
    loro_doc_free(d);

    /* --- G6.3: doc method tail & commit options --- */
    LoroDoc* g3 = loro_doc_new();
    CHECK(g3 != NULL);
    CHECK(loro_doc_set_peer_id(g3, 11) == LORO_OK);

    /* commit_with applies the message + timestamp (seen via change metadata). */
    LoroText* g3t = loro_doc_get_text(g3, "t", 1);
    CHECK(loro_text_insert(g3t, 0, "x", 1) == LORO_OK);
    LoroCommitOptions opts = {0};
    opts.message = "hi";
    opts.message_len = 2;
    opts.timestamp = 555;
    opts.has_timestamp = true;
    opts.immediate_renew = true;
    CHECK(loro_doc_commit_with(g3, opts) == LORO_OK);

    LoroId g3id = {11, 0};
    LoroChangeMetaOwned* g3meta = NULL;
    CHECK(loro_doc_get_change(g3, g3id, &g3meta) == LORO_OK);
    {
        const LoroChangeMeta* r = loro_change_meta_owned_as_ref(g3meta);
        LoroBytes msg = {0};
        CHECK(loro_change_meta_message(r, &msg) == LORO_OK);
        CHECK(bytes_eq(&msg, "hi"));
        loro_bytes_free(msg);
        CHECK(loro_change_meta_timestamp(r) == 555);
    }
    loro_change_meta_owned_free(g3meta);

    /* set_next_commit_origin + clear_next_commit_options are callable. */
    CHECK(loro_doc_set_next_commit_origin(g3, "ui", 2) == LORO_OK);
    CHECK(loro_doc_clear_next_commit_options(g3) == LORO_OK);

    /* get_container by id (type-erased) returns the right kind; bogus id -> NULL. */
    LoroBytes g3cid = {0};
    CHECK(loro_text_id(g3t, &g3cid) == LORO_OK);
    LoroContainer* anyc = loro_doc_get_container(g3, (const char*)g3cid.data, g3cid.len);
    CHECK(anyc != NULL);
    CHECK(loro_container_type(anyc) == LORO_CONTAINER_TEXT);
    loro_container_free(anyc);
    loro_bytes_free(g3cid);
    CHECK(loro_doc_get_container(g3, "cid:99@99:Map", 13) == NULL);

    /* deep_value_with_id JSON mentions the root text key. */
    LoroBytes dvid = {0};
    CHECK(loro_doc_get_deep_value_with_id_json(g3, &dvid) == LORO_OK);
    CHECK(bytes_contains(&dvid, "\"t\"") == 1);
    loro_bytes_free(dvid);

    /* attach / detach toggle is_detached. */
    CHECK(loro_doc_is_detached(g3) == false);
    CHECK(loro_doc_detach(g3) == LORO_OK);
    CHECK(loro_doc_is_detached(g3) == true);
    CHECK(loro_doc_attach(g3) == LORO_OK);
    CHECK(loro_doc_is_detached(g3) == false);

    /* find_id_spans_between: forward holds the new ops under peer 11. */
    LoroFrontiers* g3f = loro_doc_state_frontiers(g3);
    CHECK(g3f != NULL);
    LoroFrontiers* g3empty = loro_frontiers_new();
    LoroBytes spans = {0};
    CHECK(loro_doc_find_id_spans_between(g3, g3empty, g3f, &spans) == LORO_OK);
    CHECK(bytes_contains(&spans, "forward") == 1);
    CHECK(bytes_contains(&spans, "\"11\"") == 1);
    loro_bytes_free(spans);
    loro_frontiers_free(g3empty);
    loro_frontiers_free(g3f);

    /* Null-handle fallbacks for the G6.3 surface. */
    CHECK(loro_doc_attach(NULL) == LORO_ERR_INVALID_ARG);
    CHECK(loro_doc_get_container(NULL, "x", 1) == NULL);
    LoroCommitOptions nopts = {0};
    CHECK(loro_doc_commit_with(NULL, nopts) == LORO_ERR_INVALID_ARG);

    /* --- G6.4: per-container uniform set & attribution --- */
    LoroDoc* d6 = loro_doc_new();
    CHECK(loro_doc_set_peer_id(d6, 77) == LORO_OK);
    LoroText* t6 = loro_doc_get_text(d6, "t", 1);
    CHECK(t6 != NULL);

    /* Uniform introspection on an attached root container. */
    CHECK(loro_text_is_attached(t6) == true);
    CHECK(loro_text_is_deleted(t6) == false);
    LoroDoc* owner = loro_text_doc(t6);
    CHECK(owner != NULL);
    loro_doc_free(owner);

    /* Per-container subscribe fires on commit, then stops once freed. */
    sub_capture cap6;
    memset(&cap6, 0, sizeof cap6);
    LoroSubscriber cb6;
    cb6.invoke = on_event;
    cb6.user_data = &cap6;
    cb6.free_user_data = NULL;
    LoroSubscription* sub6 = loro_text_subscribe(t6, cb6);
    CHECK(sub6 != NULL);
    CHECK(loro_text_insert(t6, 0, "hi", 2) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);
    CHECK(cap6.calls >= 1);
    int calls_after = cap6.calls;
    loro_subscription_free(sub6);
    CHECK(loro_text_insert(t6, 0, "x", 1) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);
    CHECK(cap6.calls == calls_after);

    /* Text attribution: editor at a unicode position. */
    uint64_t tpeer = 0;
    CHECK(loro_text_get_editor_at_unicode_pos(t6, 0, &tpeer) == true);
    CHECK(tpeer == 77);
    CHECK(loro_text_get_editor_at_unicode_pos(t6, 1000, &tpeer) == false);

    /* Map attribution: last editor of a key. */
    LoroMap* m6 = loro_doc_get_map(d6, "m", 1);
    CHECK(loro_map_insert(m6, "k", 1, "1", 1) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);
    CHECK(loro_map_is_attached(m6) == true);
    uint64_t mpeer = 0;
    CHECK(loro_map_get_last_editor(m6, "k", 1, &mpeer) == true);
    CHECK(mpeer == 77);
    CHECK(loro_map_get_last_editor(m6, "absent", 6, &mpeer) == false);

    /* MovableList attribution: creator / last mover / last editor. */
    LoroMovableList* ml6 = loro_doc_get_movable_list(d6, "ml", 2);
    CHECK(loro_movable_list_insert(ml6, 0, "1", 1) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);
    uint64_t cpeer = 0, vpeer = 0, epeer = 0;
    CHECK(loro_movable_list_get_creator_at(ml6, 0, &cpeer) == true);
    CHECK(loro_movable_list_get_last_mover_at(ml6, 0, &vpeer) == true);
    CHECK(loro_movable_list_get_last_editor_at(ml6, 0, &epeer) == true);
    CHECK(cpeer == 77 && vpeer == 77 && epeer == 77);
    /* Out-of-range positional queries clamp in loro's movable list, so use a null handle
     * for the negative path instead. */
    CHECK(loro_movable_list_get_creator_at(NULL, 0, &cpeer) == false);

    /* Tree attribution: last move id of a node (creation is the initial move). */
    LoroTree* tr6 = loro_doc_get_tree(d6, "tr", 2);
    LoroTreeID node;
    CHECK(loro_tree_create(tr6, NULL, &node) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);
    LoroId mid;
    CHECK(loro_tree_get_last_move_id(tr6, node, &mid) == true);
    CHECK(mid.peer == 77);
    LoroTreeID bogus_node = {999999u, 4242};
    CHECK(loro_tree_get_last_move_id(tr6, bogus_node, &mid) == false);

    /* A detached container: not attached, no doc, no subscription. */
    LoroContainer* det_c = loro_container_new(LORO_CONTAINER_TEXT);
    CHECK(det_c != NULL);
    LoroText* det = loro_container_get_text(det_c);
    CHECK(det != NULL);
    CHECK(loro_text_is_attached(det) == false);
    CHECK(loro_text_doc(det) == NULL);
    LoroSubscription* det_sub = loro_text_subscribe(det, cb6);
    CHECK(det_sub == NULL);

    /* --- G6.5: tree extras --- */
    LoroTreeID child6;
    CHECK(loro_tree_create(tr6, &node, &child6) == LORO_OK);
    CHECK(loro_doc_commit(d6) == LORO_OK);

    /* parent classification: root vs node vs nonexistent. */
    LoroTreeParentKind kind6;
    LoroTreeID pnode6;
    CHECK(loro_tree_parent(tr6, node, &kind6, &pnode6) == true);
    CHECK(kind6 == LORO_TREE_PARENT_ROOT);
    CHECK(loro_tree_parent(tr6, child6, &kind6, &pnode6) == true);
    CHECK(kind6 == LORO_TREE_PARENT_NODE);
    CHECK(pnode6.peer == node.peer && pnode6.counter == node.counter);
    CHECK(loro_tree_parent(tr6, bogus_node, &kind6, &pnode6) == false);

    /* bulk JSON forms emit {peer,counter} objects. */
    LoroBytes rb = {0};
    CHECK(loro_tree_roots_json(tr6, &rb) == LORO_OK);
    CHECK(bytes_contains(&rb, "peer"));
    loro_bytes_free(rb);
    LoroBytes nb = {0};
    CHECK(loro_tree_nodes_json(tr6, &nb) == LORO_OK);
    CHECK(bytes_contains(&nb, "counter"));
    loro_bytes_free(nb);
    LoroBytes cb_kids = {0};
    CHECK(loro_tree_children_json(tr6, &node, &cb_kids) == LORO_OK);
    CHECK(bytes_contains(&cb_kids, "peer"));
    loro_bytes_free(cb_kids);
    LoroBytes cb_root = {0};
    CHECK(loro_tree_children_json(tr6, NULL, &cb_root) == LORO_OK);
    loro_bytes_free(cb_root);
    LoroBytes cb_bad = {0};
    CHECK(loro_tree_children_json(tr6, &bogus_node, &cb_bad) == LORO_ERR_NOT_FOUND);

    /* value with metadata, then disable the fractional index. */
    LoroBytes vm6 = {0};
    CHECK(loro_tree_get_value_with_meta_json(tr6, &vm6) == LORO_OK);
    CHECK(vm6.len > 0);
    loro_bytes_free(vm6);
    CHECK(loro_tree_disable_fractional_index(tr6) == LORO_OK);
    CHECK(loro_tree_is_fractional_index_enabled(tr6) == false);

    /* --- G6.5: map ensure_mergeable_* (one per container type) --- */
    LoroText* mt = loro_map_ensure_mergeable_text(m6, "mt", 2);
    CHECK(mt != NULL);
    CHECK(loro_text_insert(mt, 0, "hey", 3) == LORO_OK);
    LoroMap* mmap = loro_map_ensure_mergeable_map(m6, "mmap", 4);
    CHECK(mmap != NULL);
    LoroList* mlist = loro_map_ensure_mergeable_list(m6, "mlist", 5);
    CHECK(mlist != NULL);
    LoroMovableList* mmov = loro_map_ensure_mergeable_movable_list(m6, "mmov", 4);
    CHECK(mmov != NULL);
    LoroTree* mtree = loro_map_ensure_mergeable_tree(m6, "mtree", 5);
    CHECK(mtree != NULL);
    LoroCounter* mctr = loro_map_ensure_mergeable_counter(m6, "mctr", 4);
    CHECK(mctr != NULL);
    CHECK(loro_doc_commit(d6) == LORO_OK);

    /* re-requesting the same mergeable key yields a usable handle. */
    LoroText* mt2 = loro_map_ensure_mergeable_text(m6, "mt", 2);
    CHECK(mt2 != NULL);

    /* a key holding a plain value is non-mergeable -> null. */
    CHECK(loro_map_insert(m6, "plainkey", 8, "1", 1) == LORO_OK);
    CHECK(loro_map_ensure_mergeable_text(m6, "plainkey", 8) == NULL);

    loro_text_free(mt2);
    loro_counter_free(mctr);
    loro_tree_free(mtree);
    loro_movable_list_free(mmov);
    loro_list_free(mlist);
    loro_map_free(mmap);
    loro_text_free(mt);

    /* Null-handle / out fallbacks for the G6.4/G6.5 surface. */
    CHECK(loro_text_is_attached(NULL) == false);
    CHECK(loro_text_get_attached(NULL) == NULL);
    CHECK(loro_text_doc(NULL) == NULL);
    CHECK(loro_text_get_editor_at_unicode_pos(NULL, 0, &tpeer) == false);
    CHECK(loro_text_get_editor_at_unicode_pos(t6, 0, NULL) == false);
    CHECK(loro_tree_get_last_move_id(NULL, node, &mid) == false);
    CHECK(loro_tree_parent(NULL, node, &kind6, &pnode6) == false);
    CHECK(loro_map_ensure_mergeable_text(NULL, "x", 1) == NULL);

    loro_text_free(det);
    loro_container_free(det_c);
    loro_tree_free(tr6);
    loro_movable_list_free(ml6);
    loro_map_free(m6);
    loro_text_free(t6);
    loro_doc_free(d6);

    loro_text_free(g3t);
    loro_doc_free(g3);

    /* ---- G6.6: VersionVector algebra & UndoManager extras ---- */
    LoroDoc* va = loro_doc_new();
    CHECK(loro_doc_set_peer_id(va, 1) == LORO_OK);
    LoroText* vat = loro_doc_get_text(va, "t", 1);
    CHECK(loro_text_insert(vat, 0, "hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(va) == LORO_OK);
    LoroDoc* vb = loro_doc_new();
    CHECK(loro_doc_set_peer_id(vb, 2) == LORO_OK);
    LoroText* vbt = loro_doc_get_text(vb, "t", 1);
    CHECK(loro_text_insert(vbt, 0, "world", 5) == LORO_OK);
    CHECK(loro_doc_commit(vb) == LORO_OK);

    LoroVersionVector* vva = loro_doc_oplog_vv(va);
    LoroVersionVector* vvb = loro_doc_oplog_vv(vb);
    CHECK(vva != NULL && vvb != NULL);
    /* concurrent: a covers peer 1, b covers peer 2 */
    CHECK(loro_version_vector_includes_vv(vva, vvb) == false);
    CHECK(loro_version_vector_merge(vva, vvb) == LORO_OK);
    CHECK(loro_version_vector_includes_vv(vva, vvb) == true);

    /* extend_to_include_vv mirrors merge */
    LoroVersionVector* vva2 = loro_doc_oplog_vv(va);
    CHECK(loro_version_vector_extend_to_include_vv(vva2, vvb) == LORO_OK);
    CHECK(loro_version_vector_includes_vv(vva2, vvb) == true);

    /* synthetic vector: set_end is exclusive, then try_update_last grows the end */
    LoroVersionVector* sv = loro_version_vector_new();
    LoroId end5 = {42, 5};
    CHECK(loro_version_vector_set_end(sv, end5) == LORO_OK);
    LoroId id4 = {42, 4};
    LoroId id5 = {42, 5};
    CHECK(loro_version_vector_includes_id(sv, id4) == true);
    CHECK(loro_version_vector_includes_id(sv, id5) == false);
    bool updated = false;
    LoroId last9 = {42, 9};
    CHECK(loro_version_vector_try_update_last(sv, last9, &updated) == LORO_OK);
    CHECK(updated == true);
    LoroId last3 = {42, 3};
    CHECK(loro_version_vector_try_update_last(sv, last3, &updated) == LORO_OK);
    CHECK(updated == false);

    /* diff + get_missing_span against an empty vector */
    LoroVersionVector* ev = loro_version_vector_new();
    LoroBytes diffb = {0};
    CHECK(loro_version_vector_diff(ev, sv, &diffb) == LORO_OK);
    CHECK(bytes_contains(&diffb, "\"forward\""));
    CHECK(bytes_contains(&diffb, "\"retreat\""));
    CHECK(bytes_contains(&diffb, "\"peer\":42"));
    loro_bytes_free(diffb);

    LoroBytes missb = {0};
    CHECK(loro_version_vector_get_missing_span(ev, sv, &missb) == LORO_OK);
    CHECK(bytes_contains(&missb, "\"counter_start\":0"));
    CHECK(bytes_contains(&missb, "\"counter_end\":10"));
    loro_bytes_free(missb);

    /* intersect_span clamps to what sv has seen for peer 42 */
    LoroIdSpan isp = {42, 0, 20};
    LoroCounterSpan ico = {0, 0};
    CHECK(loro_version_vector_intersect_span(sv, isp, &ico) == true);
    CHECK(ico.start == 0 && ico.end == 10);
    LoroIdSpan absent = {999, 0, 5};
    CHECK(loro_version_vector_intersect_span(sv, absent, &ico) == false);

    /* null-handle fallbacks */
    CHECK(loro_version_vector_merge(NULL, vvb) == LORO_ERR_INVALID_ARG);
    CHECK(loro_version_vector_intersect_span(NULL, isp, &ico) == false);

    loro_version_vector_free(ev);
    loro_version_vector_free(sv);
    loro_version_vector_free(vva2);
    loro_version_vector_free(vvb);
    loro_version_vector_free(vva);

    /* UndoManager extras: peer() and the top undo-stack value */
    LoroUndoManager* um = loro_undo_manager_new(va);
    CHECK(um != NULL);
    CHECK(loro_undo_manager_peer(um) == loro_doc_peer_id(va));
    LoroBytes uvb = {0};
    /* this manager has recorded nothing since creation -> empty undo stack */
    CHECK(loro_undo_manager_top_undo_value_json(um, &uvb) == false);
    /* a fresh local edit becomes an undoable item with the default (null) meta value */
    CHECK(loro_text_insert(vat, 5, "!", 1) == LORO_OK);
    CHECK(loro_doc_commit(va) == LORO_OK);
    CHECK(loro_undo_manager_top_undo_value_json(um, &uvb) == true);
    CHECK(bytes_contains(&uvb, "null"));
    loro_bytes_free(uvb);
    /* null-handle fallbacks */
    CHECK(loro_undo_manager_peer(NULL) == 0);
    LoroBytes uvb2 = {0};
    CHECK(loro_undo_manager_top_undo_value_json(NULL, &uvb2) == false);
    CHECK(loro_undo_manager_top_undo_value_json(um, NULL) == false);

    loro_undo_manager_free(um);
    loro_text_free(vbt);
    loro_text_free(vat);
    loro_doc_free(vb);
    loro_doc_free(va);
}

/* UndoManager pause/resume (loro 1.16): a paused checkout round-trip leaves the undo/redo
 * stacks intact, mirroring loro's pause_preserves_undo_stacks_across_checkout; without the
 * pause, a checkout clears them. */
static int text_is(LoroText* t, const char* s) {
    LoroBytes b = {0};
    if (loro_text_to_string(t, &b) != LORO_OK) return 0;
    int eq = bytes_eq(&b, s);
    loro_bytes_free(b);
    return eq;
}

static void test_undo_pause_c(void) {
    LoroDoc* doc = loro_doc_new();
    CHECK(loro_doc_set_peer_id(doc, 1) == LORO_OK);
    LoroUndoManager* um = loro_undo_manager_new(doc);
    CHECK(um != NULL);
    LoroText* t = loro_doc_get_text(doc, "text", 4);

    CHECK(loro_text_insert(t, 0, "Hello", 5) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_text_insert(t, 5, " World", 6) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);
    CHECK(loro_undo_manager_can_undo(um));

    CHECK(loro_undo_manager_is_paused(um) == false);
    CHECK(loro_undo_manager_pause(um) == LORO_OK);
    CHECK(loro_undo_manager_is_paused(um) == true);
    LoroFrontiers* empty = loro_frontiers_new();
    CHECK(loro_doc_checkout(doc, empty) == LORO_OK);
    CHECK(text_is(t, ""));
    CHECK(loro_doc_checkout_to_latest(doc) == LORO_OK);
    CHECK(text_is(t, "Hello World"));
    CHECK(loro_undo_manager_resume(um) == LORO_OK);
    CHECK(loro_undo_manager_is_paused(um) == false);

    bool applied = false;
    CHECK(loro_undo_manager_can_undo(um));
    CHECK(loro_undo_manager_undo(um, &applied) == LORO_OK && applied);
    CHECK(text_is(t, "Hello"));
    CHECK(loro_undo_manager_undo(um, &applied) == LORO_OK && applied);
    CHECK(text_is(t, ""));
    CHECK(loro_undo_manager_can_redo(um));
    CHECK(loro_undo_manager_redo(um, &applied) == LORO_OK && applied);
    CHECK(loro_undo_manager_redo(um, &applied) == LORO_OK && applied);
    CHECK(text_is(t, "Hello World"));

    /* unpaused checkout still clears both stacks */
    CHECK(loro_doc_checkout(doc, empty) == LORO_OK);
    CHECK(!loro_undo_manager_can_undo(um));
    CHECK(!loro_undo_manager_can_redo(um));

    /* null-handle fallbacks */
    CHECK(loro_undo_manager_pause(NULL) == LORO_ERR_INVALID_ARG);
    CHECK(loro_undo_manager_resume(NULL) == LORO_ERR_INVALID_ARG);
    CHECK(loro_undo_manager_is_paused(NULL) == false);

    loro_frontiers_free(empty);
    loro_undo_manager_free(um);
    loro_text_free(t);
    loro_doc_free(doc);
}

/* Typed reads with no JSON: loro_map_get_value (shallow), loro_value_map_for_each and
 * loro_text_for_each_delta. Each visitor stops early when invoke returns false, and its
 * free_user_data runs exactly once per call, errors included. */
typedef struct {
    int entries;
    int stop_after; /* 0 = never stop */
    int saw_n;
    int saw_content;
    int frees;
} map_visit;

static bool map_visit_entry(const char* key, uintptr_t key_len, const LoroValue* value,
                            void* user_data) {
    map_visit* v = (map_visit*)user_data;
    v->entries++;
    if (key_len == 1 && memcmp(key, "n", 1) == 0) {
        int64_t i = 0;
        v->saw_n = loro_value_as_i64(value, &i) && i == 42;
    } else if (key_len == 7 && memcmp(key, "content", 7) == 0) {
        v->saw_content = loro_value_get_type(value) == LORO_VALUE_CONTAINER;
    }
    return v->stop_after == 0 || v->entries < v->stop_after;
}

static void map_visit_free(void* user_data) { ((map_visit*)user_data)->frees++; }

typedef struct {
    int segments;
    int stop_after; /* 0 = never stop */
    char text[64];
    int bold_first;
    int plain_second;
    int frees;
} delta_visit;

static bool delta_visit_segment(const char* insert, uintptr_t insert_len,
                                const LoroValue* attributes, void* user_data) {
    delta_visit* v = (delta_visit*)user_data;
    size_t used = strlen(v->text);
    if (used + insert_len < sizeof v->text) {
        memcpy(v->text + used, insert, insert_len);
        v->text[used + insert_len] = '\0';
    }
    if (v->segments == 0 && attributes != NULL) {
        LoroValue* bold = loro_value_map_get(attributes, "bold", 4);
        bool b = false;
        v->bold_first = bold != NULL && loro_value_as_bool(bold, &b) && b;
        loro_value_free(bold);
    } else if (v->segments == 1) {
        v->plain_second = attributes == NULL;
    }
    v->segments++;
    return v->stop_after == 0 || v->segments < v->stop_after;
}

static void delta_visit_free(void* user_data) { ((delta_visit*)user_data)->frees++; }

static void test_typed_reads_c(void) {
    LoroDoc* doc = loro_doc_new();
    LoroMap* m = loro_doc_get_map(doc, "m", 1);
    CHECK(loro_map_insert(m, "n", 1, "42", 2) == LORO_OK);
    CHECK(loro_map_insert(m, "s", 1, "\"hi\"", 4) == LORO_OK);
    LoroContainer* content =
        loro_map_insert_container(m, "content", 7, loro_container_new(LORO_CONTAINER_TEXT));
    CHECK(content != NULL);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    LoroValue* shallow = loro_map_get_value(m);
    CHECK(shallow != NULL);
    CHECK(loro_value_get_type(shallow) == LORO_VALUE_MAP);
    CHECK(loro_value_map_len(shallow) == 3);

    map_visit all = {0};
    LoroValueMapVisitor all_visitor = {map_visit_entry, &all, map_visit_free};
    CHECK(loro_value_map_for_each(shallow, all_visitor) == LORO_OK);
    CHECK(all.entries == 3 && all.saw_n && all.saw_content && all.frees == 1);

    map_visit first = {0};
    first.stop_after = 1;
    LoroValueMapVisitor first_visitor = {map_visit_entry, &first, map_visit_free};
    CHECK(loro_value_map_for_each(shallow, first_visitor) == LORO_OK);
    CHECK(first.entries == 1 && first.frees == 1);

    LoroValue* num = loro_value_new_i64(1);
    map_visit bad = {0};
    LoroValueMapVisitor bad_visitor = {map_visit_entry, &bad, map_visit_free};
    CHECK(loro_value_map_for_each(num, bad_visitor) == LORO_ERR_INVALID_ARG);
    CHECK(loro_value_map_for_each(NULL, bad_visitor) == LORO_ERR_INVALID_ARG);
    CHECK(bad.entries == 0 && bad.frees == 2);
    CHECK(loro_map_get_value(NULL) == NULL);
    loro_value_free(num);
    loro_value_free(shallow);

    LoroText* t = loro_doc_get_text(doc, "t", 1);
    CHECK(loro_text_insert(t, 0, "Hello world", 11) == LORO_OK);
    CHECK(loro_text_mark(t, 0, 5, "bold", 4, "true", 4) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    delta_visit whole = {0};
    LoroTextDeltaVisitor whole_visitor = {delta_visit_segment, &whole, delta_visit_free};
    CHECK(loro_text_for_each_delta(t, whole_visitor) == LORO_OK);
    CHECK(whole.segments == 2 && strcmp(whole.text, "Hello world") == 0);
    CHECK(whole.bold_first && whole.plain_second && whole.frees == 1);

    delta_visit head = {0};
    head.stop_after = 1;
    LoroTextDeltaVisitor head_visitor = {delta_visit_segment, &head, delta_visit_free};
    CHECK(loro_text_for_each_delta(t, head_visitor) == LORO_OK);
    CHECK(head.segments == 1 && strcmp(head.text, "Hello") == 0 && head.frees == 1);

    delta_visit none = {0};
    LoroTextDeltaVisitor none_visitor = {delta_visit_segment, &none, delta_visit_free};
    CHECK(loro_text_for_each_delta(NULL, none_visitor) == LORO_ERR_INVALID_ARG);
    CHECK(none.segments == 0 && none.frees == 1);

    loro_text_free(t);
    loro_container_free(content);
    loro_map_free(m);
    loro_doc_free(doc);
}

/* loro 1.17: remote updates still waiting for their dependencies are left out of snapshots;
 * loro_doc_export_pending_updates saves them so a restored doc keeps waiting for them. Mirrors
 * the example on loro's LoroDoc::export_pending_updates. */
static void test_pending_updates_c(void) {
    LoroDoc* source = loro_doc_new();
    CHECK(loro_doc_set_peer_id(source, 1) == LORO_OK);
    LoroText* st = loro_doc_get_text(source, "text", 4);
    CHECK(loro_text_insert(st, 0, "base|", 5) == LORO_OK);
    CHECK(loro_doc_commit(source) == LORO_OK);
    LoroBytes base = {0};
    CHECK(loro_doc_export_snapshot(source, &base) == LORO_OK);
    LoroVersionVector* before_a = loro_doc_oplog_vv(source);
    CHECK(loro_text_insert(st, 5, "A", 1) == LORO_OK);
    CHECK(loro_doc_commit(source) == LORO_OK);
    LoroBytes a = {0};
    CHECK(loro_doc_export_updates_from(source, before_a, &a) == LORO_OK);
    LoroVersionVector* before_b = loro_doc_oplog_vv(source);
    CHECK(loro_text_insert(st, 6, "B", 1) == LORO_OK);
    CHECK(loro_doc_commit(source) == LORO_OK);
    LoroBytes b = {0};
    CHECK(loro_doc_export_updates_from(source, before_b, &b) == LORO_OK);

    LoroDoc* receiver = loro_doc_new();
    CHECK(loro_doc_import(receiver, base.data, base.len) == LORO_OK);
    LoroBytes none = {0};
    bool has = true;
    CHECK(loro_doc_export_pending_updates(receiver, &none, &has) == LORO_OK);
    CHECK(!has && none.len == 0);
    loro_bytes_free(none);

    /* B arrives before A, so it waits. */
    CHECK(loro_doc_import(receiver, b.data, b.len) == LORO_OK);
    LoroBytes snapshot = {0};
    CHECK(loro_doc_export_snapshot(receiver, &snapshot) == LORO_OK);
    LoroBytes pending = {0};
    CHECK(loro_doc_export_pending_updates(receiver, &pending, &has) == LORO_OK);
    CHECK(has && pending.len > 0);
    LoroBytes again = {0};
    CHECK(loro_doc_export_pending_updates(receiver, &again, NULL) == LORO_OK);
    CHECK(again.len == pending.len);
    loro_bytes_free(again);

    /* Restore: the snapshot alone has lost B; importing the pending bytes brings it back. */
    LoroDoc* restored = loro_doc_new();
    LoroText* rt = loro_doc_get_text(restored, "text", 4);
    CHECK(loro_doc_import(restored, snapshot.data, snapshot.len) == LORO_OK);
    CHECK(loro_doc_import(restored, pending.data, pending.len) == LORO_OK);
    CHECK(text_is(rt, "base|"));
    LoroBytes still = {0};
    CHECK(loro_doc_export_pending_updates(restored, &still, &has) == LORO_OK && has);
    loro_bytes_free(still);
    CHECK(loro_doc_import(restored, a.data, a.len) == LORO_OK);
    CHECK(text_is(rt, "base|AB"));
    LoroBytes after = {0};
    CHECK(loro_doc_export_pending_updates(restored, &after, &has) == LORO_OK);
    CHECK(!has && after.len == 0);
    loro_bytes_free(after);

    LoroBytes unused = {0};
    CHECK(loro_doc_export_pending_updates(NULL, &unused, &has) == LORO_ERR_INVALID_ARG);
    CHECK(loro_doc_export_pending_updates(restored, NULL, &has) == LORO_ERR_INVALID_ARG);

    loro_bytes_free(pending);
    loro_bytes_free(snapshot);
    loro_bytes_free(b);
    loro_bytes_free(a);
    loro_bytes_free(base);
    loro_version_vector_free(before_b);
    loro_version_vector_free(before_a);
    loro_text_free(rt);
    loro_text_free(st);
    loro_doc_free(restored);
    loro_doc_free(receiver);
    loro_doc_free(source);
}

/* Whether root map "m" of `doc` holds binary (not a list) under "bin". */
static int bin_is_binary(LoroDoc* doc) {
    LoroMap* m = loro_doc_get_map(doc, "m", 1);
    LoroValue* v = loro_map_get_value(m);
    LoroValue* bin = v ? loro_value_map_get(v, "bin", 3) : NULL;
    int ok = bin != NULL && loro_value_get_type(bin) == LORO_VALUE_BINARY;
    loro_value_free(bin);
    loro_value_free(v);
    loro_map_free(m);
    return ok;
}

/* Whether root map "relay" of `doc` has `n` entries. */
static int relay_len_is(LoroDoc* doc, uintptr_t n) {
    LoroMap* m = loro_doc_get_map(doc, "relay", 5);
    int ok = loro_map_len(m) == n;
    loro_map_free(m);
    return ok;
}

/* loro 1.17: binary sent through JSON updates comes back as a list of numbers. When the JSON
 * receiver passes that history on as binary, an exact import into a doc that has the original
 * ops is rejected; LORO_IMPORT_HISTORY_JSON_LOSSY accepts it and keeps the receiver's values. */
static void test_import_history_mode_c(void) {
    LoroDoc* a = loro_doc_new();
    CHECK(loro_doc_set_peer_id(a, 1) == LORO_OK);
    LoroMap* am = loro_doc_get_map(a, "m", 1);
    const uint8_t raw[3] = {0, 1, 255};
    LoroValue* bin = loro_value_new_binary(raw, 3);
    CHECK(loro_map_insert_value(am, "bin", 3, bin) == LORO_OK);
    loro_value_free(bin);
    CHECK(loro_doc_commit(a) == LORO_OK);
    LoroBytes prefix = {0};
    CHECK(loro_doc_export_updates(a, &prefix) == LORO_OK);

    LoroVersionVector* empty = loro_version_vector_new();
    LoroVersionVector* a_vv = loro_doc_oplog_vv(a);
    LoroBytes json = {0};
    CHECK(loro_doc_export_json_updates(a, empty, a_vv, &json) == LORO_OK);
    LoroDoc* relay = loro_doc_new();
    CHECK(loro_doc_set_peer_id(relay, 2) == LORO_OK);
    CHECK(loro_doc_import_json_updates(relay, (const char*)json.data, json.len) == LORO_OK);
    CHECK(!bin_is_binary(relay));
    LoroMap* rm = loro_doc_get_map(relay, "relay", 5);
    CHECK(loro_map_insert(rm, "extra", 5, "1", 1) == LORO_OK);
    CHECK(loro_doc_commit(relay) == LORO_OK);
    LoroBytes blob = {0};
    CHECK(loro_doc_export_updates(relay, &blob) == LORO_OK);

    /* Exact (the default, and LORO_IMPORT_HISTORY_EXACT) rejects it and changes nothing. */
    LoroDoc* exact = loro_doc_new();
    CHECK(loro_doc_import(exact, prefix.data, prefix.len) == LORO_OK);
    CHECK(loro_doc_import(exact, blob.data, blob.len) != LORO_OK);
    LoroImportStatus* st = NULL;
    CHECK(loro_doc_import_with_history_mode(exact, blob.data, blob.len, "", 0,
                                            LORO_IMPORT_HISTORY_EXACT, &st) != LORO_OK);
    CHECK(st == NULL);
    CHECK(relay_len_is(exact, 0) && bin_is_binary(exact));

    LoroDoc* lossy = loro_doc_new();
    CHECK(loro_doc_import(lossy, prefix.data, prefix.len) == LORO_OK);
    CHECK(loro_doc_import_with_history_mode(lossy, blob.data, blob.len, "relay", 5,
                                            LORO_IMPORT_HISTORY_JSON_LOSSY, &st) == LORO_OK);
    CHECK(st != NULL && loro_import_status_success_len(st) > 0);
    loro_import_status_free(st);
    CHECK(relay_len_is(lossy, 1) && bin_is_binary(lossy));

    LoroDoc* batch = loro_doc_new();
    CHECK(loro_doc_import(batch, prefix.data, prefix.len) == LORO_OK);
    const uint8_t* datas[1] = {blob.data};
    size_t lens[1] = {blob.len};
    CHECK(loro_doc_import_batch_with_history_mode(batch, datas, lens, 1, LORO_IMPORT_HISTORY_EXACT,
                                                  NULL) != LORO_OK);
    CHECK(loro_doc_import_batch_with_history_mode(batch, datas, lens, 1,
                                                  LORO_IMPORT_HISTORY_JSON_LOSSY, NULL) == LORO_OK);
    CHECK(relay_len_is(batch, 1) && bin_is_binary(batch));
    CHECK(loro_doc_import_batch_with_history_mode(batch, NULL, lens, 1,
                                                  LORO_IMPORT_HISTORY_JSON_LOSSY,
                                                  NULL) == LORO_ERR_INVALID_ARG);

    loro_bytes_free(blob);
    loro_bytes_free(json);
    loro_bytes_free(prefix);
    loro_version_vector_free(a_vv);
    loro_version_vector_free(empty);
    loro_map_free(rm);
    loro_map_free(am);
    loro_doc_free(batch);
    loro_doc_free(lossy);
    loro_doc_free(exact);
    loro_doc_free(relay);
    loro_doc_free(a);
}

static void test_decoded_value_cache_c(void) {
    LoroDoc* doc = loro_doc_new();
    CHECK(loro_doc_decoded_value_cache_limit(doc) == 2048);
    loro_doc_set_decoded_value_cache_limit(doc, 16);
    CHECK(loro_doc_decoded_value_cache_limit(doc) == 16);
    LoroDoc* fork = loro_doc_fork(doc);
    CHECK(loro_doc_decoded_value_cache_limit(fork) == 16);
    loro_doc_set_decoded_value_cache_limit(fork, 0);
    CHECK(loro_doc_decoded_value_cache_limit(doc) == 16);
    CHECK(loro_doc_decoded_value_cache_limit(NULL) == 0);
    loro_doc_set_decoded_value_cache_limit(NULL, 1); /* no-op */
    loro_doc_free(fork);
    loro_doc_free(doc);
}

/* Reads tests/fixtures/<name> into `*out` (free with free()); returns its size, or 0 on failure. */
static size_t read_fixture(const char* name, uint8_t** out) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", LORO_TEST_FIXTURES, name);
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 ? (uint8_t*)malloc((size_t)n) : NULL;
    size_t got = buf ? fread(buf, 1, (size_t)n, f) : 0;
    fclose(f);
    if (got == 0 || got != (size_t)n) {
        free(buf);
        return 0;
    }
    *out = buf;
    return got;
}

/* loro 1.17: reads that need a stored change block that cannot be parsed report it
 * (LORO_ERR_DECODE, or null from functions returning a handle) instead of panicking or skipping
 * the block. The fixture's block holding 1@1 is truncated. */
static void test_unreadable_history_c(void) {
    uint8_t* bytes = NULL;
    size_t len = read_fixture("unparsable-history.snapshot.bin", &bytes);
    CHECK(len > 0);
    if (len == 0) return;
    LoroDoc* doc = loro_doc_new();
    CHECK(loro_doc_import(doc, bytes, len) == LORO_OK);
    free(bytes);

    /* At the tip the state version comes from the cached oplog version, so it still works. */
    LoroVersionVector* state = loro_doc_state_vv(doc);
    CHECK(state != NULL);
    loro_version_vector_free(state);

    LoroId damaged = {1, 1};
    LoroFrontiers* at = loro_frontiers_new();
    CHECK(loro_frontiers_push(at, damaged) == LORO_OK);
    CHECK(loro_doc_frontiers_to_vv(doc, at) == NULL);
    CHECK(loro_last_error_message() != NULL &&
          strstr(loro_last_error_message(), "ecode") != NULL);
    CHECK(loro_doc_fork_at(doc, at) == NULL);

    LoroVersionVector* start = loro_version_vector_new();
    LoroId p1 = {1, 2};
    LoroId p2 = {2, 1};
    CHECK(loro_version_vector_set_end(start, p1) == LORO_OK);
    CHECK(loro_version_vector_set_end(start, p2) == LORO_OK);
    LoroVersionVector* end = loro_doc_oplog_vv(doc);
    LoroBytes json = {0};
    CHECK(loro_doc_export_json_updates(doc, start, end, &json) == LORO_ERR_DECODE);
    CHECK(loro_doc_export_json_updates_without_peer_compression(doc, start, end, &json) ==
          LORO_ERR_DECODE);

    LoroChangeMetaOwned* meta = NULL;
    CHECK(loro_doc_get_change(doc, damaged, &meta) == LORO_ERR_DECODE);
    CHECK(meta == NULL);

    /* The doc stays usable. */
    LoroText* t = loro_doc_get_text(doc, "text", 4);
    CHECK(loro_text_insert(t, 0, "still works", 11) == LORO_OK);
    CHECK(loro_doc_commit(doc) == LORO_OK);

    loro_text_free(t);
    loro_version_vector_free(end);
    loro_version_vector_free(start);
    loro_frontiers_free(at);
    loro_doc_free(doc);
}

int main(void) {
    CHECK(loro_version() != NULL);
    test_snapshot_round_trip();
    test_free_ordering();
    test_error_path();
    test_containers();
    test_subscribe();
    test_advanced_c();
    test_richtext_c();
    test_cursor_c();
    test_json_sync_c();
    test_diff_c();
    test_get_by_path_c();
    test_g6_c();
    test_undo_pause_c();
    test_typed_reads_c();
    test_pending_updates_c();
    test_import_history_mode_c();
    test_decoded_value_cache_c();
    test_unreadable_history_c();

    if (failures == 0) {
        puts("test_c_only: OK");
        return 0;
    }
    fprintf(stderr, "test_c_only: %d failure(s)\n", failures);
    return 1;
}
