/* A plain `cargo build` has no C, so build.rs downgrades the missing `app_main` to a warning
   (lld reports it before any ASSERT) and this message becomes the error. */
ASSERT(0, "no firmware C: build with `pio run` in a node's project folder");
