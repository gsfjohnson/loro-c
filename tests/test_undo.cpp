// Exercises UndoManager: record_new_checkpoint / undo / redo / counts /
// can_undo / can_redo / pause / resume / is_paused.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run_pause();
bool run_noop_steps();

bool run() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto undo = loro::UndoManager::init(doc);

    auto text = doc->get_text(root("body"));

    text->insert(0, "hello");
    doc->commit();
    undo->record_new_checkpoint();

    text->insert(text->len_unicode(), " world");
    doc->commit();
    undo->record_new_checkpoint();

    if (text->to_string() != "hello world") {
        return fail("expected 'hello world' before undo");
    }

    if (!undo->can_undo()) return fail("can_undo should be true");
    if (undo->undo_count() < 1) return fail("undo_count should be >= 1");

    if (!undo->undo()) return fail("undo() returned false");
    if (text->to_string() != "hello") {
        return fail("after first undo, text should be 'hello'");
    }

    if (!undo->can_redo()) return fail("can_redo should be true after undo");
    if (!undo->redo()) return fail("redo() returned false");
    if (text->to_string() != "hello world") {
        return fail("after redo, text should be 'hello world'");
    }

    undo->set_max_undo_steps(50);
    return run_pause();
}

// pause()/resume()/is_paused(), mirroring loro's undo_test.rs
// (undo_while_paused_does_not_leak_processing_flag,
// paused_local_edits_are_not_folded_into_next_undo).
bool run_pause() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto undo = loro::UndoManager::init(doc);
    auto text = doc->get_text(root("text"));

    if (undo->is_paused()) return fail("fresh manager should not be paused");

    text->insert(0, "A");
    doc->commit();

    // undo() is a no-op while paused and must not wedge the manager
    undo->pause();
    if (!undo->is_paused()) return fail("is_paused should be true after pause");
    if (undo->undo()) return fail("undo() while paused should return false");
    if (text->to_string() != "A") return fail("undo while paused changed the text");

    // a local edit while paused is not recorded as its own step...
    text->insert(1, "B");
    doc->commit();
    undo->resume();
    if (undo->is_paused()) return fail("is_paused should be false after resume");

    // ...nor folded into the next recorded one
    text->insert(2, "C");
    doc->commit();
    if (text->to_string() != "ABC") return fail("expected 'ABC'");
    if (!undo->undo()) return fail("undo of 'C' returned false");
    if (text->to_string() != "AB") return fail("first undo should leave 'AB'");
    if (!undo->undo()) return fail("undo of 'A' returned false");
    if (text->to_string() != "B") return fail("second undo should leave 'B'");
    if (undo->can_undo()) return fail("the paused edit must not be undoable");
    return run_noop_steps();
}

// Since loro 1.17 each undo() consumes at most one step, and a step that remote edits turned
// into a no-op returns false while earlier steps remain, mirroring loro's
// consecutive_noops_consume_one_item_and_notify_once.
bool run_noop_steps() {
    auto doc = loro::LoroDoc::init();
    doc->set_peer_id(1);
    auto undo = loro::UndoManager::init(doc);
    undo->set_merge_interval(0);
    auto map = doc->get_map(root("map"));
    map->insert("prior", i64_value(42));
    doc->commit();
    auto text = doc->get_text(root("text"));
    text->insert(0, "a");
    doc->commit();
    text->insert(1, "b");
    doc->commit();

    // Another peer deletes "ab", so undoing either insert has nothing left to do.
    auto other = loro::LoroDoc::init();
    other->import(doc->export_snapshot());
    other->set_peer_id(2);
    other->get_text(root("text"))->delete_(0, 2);
    other->commit();
    doc->import(other->export_updates(loro::VersionVector::init()));
    if (undo->undo_count() != 3) return fail("expected three undo steps");

    for (uint32_t remaining : {2u, 1u}) {
        if (undo->undo()) return fail("undoing a no-op step should return false");
        if (undo->undo_count() != remaining) return fail("undo consumed more than one step");
        if (map->len() != 1) return fail("a no-op undo changed the map");
    }
    if (!undo->undo()) return fail("undo of 'prior' should change the doc");
    if (map->len() != 0) return fail("undo of 'prior' did not remove it");
    if (undo->can_undo()) return fail("all steps should be consumed");
    if (!undo->redo()) return fail("redo of 'prior' should change the doc");
    if (map->len() != 1) return fail("redo did not restore 'prior'");
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
