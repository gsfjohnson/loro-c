// This file just tests which methods exist on LoroText
fn test_methods(text: &loro::LoroText) {
    // Try calling methods to see which ones exist
    let _ = text.mark(0, 0, "key", &loro::LoroValue::Null); // test mark
    let _ = text.unmark(0, 0, "key"); // test unmark
    let _ = text.get_richtext_value(); // test get_richtext_value
    let _ = text.to_delta(); // test to_delta
    let _ = text.apply_delta(&[]); // test apply_delta
    let _ = text.splice(0, 0, "text"); // test splice
    let _ = text.slice(0, 0); // test slice
    let _ = text.char_at(0); // test char_at
    let _ = text.insert_utf16(0, "text"); // test insert_utf16
    let _ = text.delete_utf16(0, 0); // test delete_utf16
    let _ = text.len_utf16(); // test len_utf16
    let _ = text.convert_pos(0, loro::PosType::Bytes, loro::PosType::Unicode); // test convert_pos
    let _ = text.push_str("text"); // test push_str
}
