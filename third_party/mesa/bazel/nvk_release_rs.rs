//! The release profile's one Rust staticlib for NVK (rust.bzl mesa_rust_release_staticlib): NAK, NIL and NVK's own
//! crate, so the standard library and the crates they share are linked once and fat LTO sees all of them.

extern crate nak_rs;
extern crate nil;
extern crate nvk_rs;
