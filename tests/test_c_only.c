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

    if (failures == 0) {
        puts("test_c_only: OK");
        return 0;
    }
    fprintf(stderr, "test_c_only: %d failure(s)\n", failures);
    return 1;
}
