// Exercises UndoManager: record_new_checkpoint / undo / redo / counts /
// can_undo / can_redo.
#include "test_helpers.hpp"

using namespace loro_test;

namespace {

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
    return true;
}

} // namespace

LORO_TEST_MAIN(run)
