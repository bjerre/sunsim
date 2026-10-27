# Installation document

`installation.json` is the source for the logical device groups and zone
behavior compiled into the firmware and WebAssembly core. It is deliberately
data-only: `look.type` and gate types name behaviors the portable C++ core
already implements; the document cannot run scripts.

After editing the document, regenerate the C++ tables:

```sh
./sunsim definition generate
```

To check that the committed generated tables match the document without
rewriting files:

```sh
./sunsim definition check
python3 -m unittest discover -s test/config
```

The generated tables still pass through the C++ `topologyOf` compile-time
checks. Keep physical Zigbee IEEE addresses in the installation's pairing
table / device settings, not in this shareable behavior document.
