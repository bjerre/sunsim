// The Rust browser simulator (sunsim_web_simulator.wasm, from rust/web), as
// the module web/simulator.ts expects of the Emscripten build it replaces:
// each exported sunsim_* function as _sunsim_*, and UTF8ToString to read
// the state's string. The wasm needs no imports.

const wasmUrl = new URL("./sunsim_web_simulator.wasm", import.meta.url);

async function wasmBytes() {
  // Node (the smoke test) reads the file; browsers fetch it.
  if (wasmUrl.protocol === "file:") {
    const fs = "node:fs/promises";
    const { readFile } = await import(/* @vite-ignore */ fs);
    return readFile(wasmUrl);
  }
  const response = await fetch(wasmUrl);
  if (!response.ok) throw new Error(`could not load ${wasmUrl}: HTTP ${response.status}`);
  return response.arrayBuffer();
}

export default async function createSunsim() {
  const { instance } = await WebAssembly.instantiate(await wasmBytes(), {});
  const exports = instance.exports;
  const decoder = new TextDecoder();
  const module = {
    UTF8ToString(pointer) {
      const bytes = new Uint8Array(exports.memory.buffer, pointer);
      const end = bytes.indexOf(0);
      return decoder.decode(bytes.subarray(0, end));
    },
  };
  for (const [name, value] of Object.entries(exports)) {
    if (name.startsWith("sunsim_")) module[`_${name}`] = value;
  }
  return module;
}
