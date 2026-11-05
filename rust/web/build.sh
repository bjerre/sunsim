#!/usr/bin/env bash
# Builds the browser simulator's wasm and puts it, with its loader, where
# web/simulator.ts imports it: sunsim_web_simulator.{wasm,js} at the
# repository's root (git-ignored), as the C++ build's Emscripten output was.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/../.." && pwd)
cargo build --manifest-path "$here/../Cargo.toml" --release --target wasm32-unknown-unknown -p sunsim-web
cp "$here/../target/wasm32-unknown-unknown/release/sunsim_web.wasm" "$root/sunsim_web_simulator.wasm"
cp "$here/js/sunsim_web_simulator.js" "$root/sunsim_web_simulator.js"
echo "built $root/sunsim_web_simulator.wasm ($(( $(stat -c %s "$root/sunsim_web_simulator.wasm") / 1024 )) KB)"
