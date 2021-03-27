#!/usr/bin/env bash

cargo generate-lockfile && \
cargo metadata > metadata.json && \
sed  -i "s#$(pwd)#CARGO_CRATE_PATH#g" metadata.json && \
bazel run @cargo_raze//:raze -- --manifest-path=$(realpath Cargo.toml) && \
cat "cbindgen_rules.txt" >> "BUILD.bazel" 
