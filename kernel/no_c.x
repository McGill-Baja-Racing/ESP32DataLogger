/* Without a node feature there is no C, so build.rs downgrades the missing `app_main` to a
   warning (lld reports it before any ASSERT) and this message becomes the error. */
ASSERT(0, "no firmware C: pick a node, e.g. `cargo run --release --features brake`");
