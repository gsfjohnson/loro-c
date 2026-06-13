//! Container handles. M1 shipped `text`; M2 adds `map`, `list`, `movable_list`,
//! `counter`, `tree`, and the type-erased `any` handle used for nested containers.

pub mod any;
pub mod counter;
pub mod list;
pub mod map;
pub mod movable_list;
pub mod text;
pub mod tree;
