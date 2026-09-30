// Exercises UndoManager: record_new_checkpoint / undo / redo / counts /
// can_undo / can_redo / pause / resume / is_paused.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

bool run_pause();

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
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
