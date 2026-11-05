# Installation document

`installation.json` is the source for the logical device groups and zone
behavior compiled into the firmware and WebAssembly core. It is deliberately
data-only: `look.type` and gate types name behaviors the core already
implements; the document cannot run scripts.

In the Rust build, `rust/ev/build.rs` reads it on every build (no generated
files to commit) and the tables it makes pass through the compile-time
`topology_of` checks, so an invalid document fails the build. Keep physical
Zigbee IEEE addresses in the installation's pairing table / device settings,
not in this shareable behavior document.

The browser simulation accepts startup-only JSON profiles in
`web/public/profiles/`. These data-only profiles regroup the supported device
roles, select their existing looks, and change daily windows. The same profile
can be uploaded to firmware without rebuilding it with
`./sunsim profile upload FILE`; the device validates it, saves it in NVS, and
restarts to apply it. `./sunsim profile clear` restores the built-in
installation. The logical role catalogue and device kinds remain fixed by the
firmware build.
