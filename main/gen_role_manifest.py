#!/usr/bin/env python3
"""Generates role_manifest.h from role_manifest.json at build time.

Keeps the role -> ext_addr table editable as plain data (fill in each
device's IEEE address from its label) without hand-editing C++ syntax.
Entries are keyed by role name and passed through as written: the C++ side
(core::DeviceRegistry::manifestOf) checks them against ev::kLayout at
compile time, so a name the layout doesn't have, or a role listed twice,
fails the build. This script never needs to know the layout.
"""
import json
import re
import sys

HEADER_TEMPLATE = """#pragma once

#include <array>
#include <cstdint>

namespace sunsim {{

// Generated from role_manifest.json by gen_role_manifest.py - do not edit by
// hand. Edit role_manifest.json instead (one IEEE/EUI-64 address per role,
// from the device's label) and rebuild. 0 = no entry. Checked against the
// role layout by core::DeviceRegistry::manifestOf.
struct RoleManifestEntry {{
    const char *role; // a role name, as ev::roleName gives it (any case)
    uint64_t    ext_addr;
}};

inline constexpr std::array<RoleManifestEntry, {count}> kRoleManifest{{{entries}}};

}} // namespace sunsim
"""


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <role_manifest.json> <output role_manifest.h>", file=sys.stderr)
        return 1

    with open(sys.argv[1], encoding="utf-8") as f:
        manifest = json.load(f)

    entries = []
    for role, raw in manifest.items():
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", role):
            print(f"error: '{role}' in {sys.argv[1]} is not a role name", file=sys.stderr)
            return 1
        try:
            value = int(raw, 16)
        except (TypeError, ValueError):
            print(f"error: role '{role}' has non-hex address: {raw!r}", file=sys.stderr)
            return 1
        entries.append(f'    {{"{role}", 0x{value:016x}}},')

    body = "{\n" + "\n".join(entries) + "\n}" if entries else ""
    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(HEADER_TEMPLATE.format(count=len(entries), entries=body))

    return 0


if __name__ == "__main__":
    sys.exit(main())
