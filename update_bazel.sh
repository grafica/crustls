#!/usr/bin/env bash

cargo generate-lockfile && \
cargo metadata --offline > metadata.json && \
sed  -i "s#$(pwd)#CARGO_CRATE_PATH#g" metadata.json && \
bazel --noworkspace_rc run @cargo_raze//:raze -- --manifest-path=$(realpath Cargo.toml) && \
cat "cbindgen_rules.txt" >> "BUILD.bazel" 
