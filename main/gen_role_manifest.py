#!/usr/bin/env python3
"""Generates role_manifest.h from role_manifest.json at build time.

Keeps the role -> ext_addr table editable as plain data (fill in each
device's IEEE address from its label) without hand-editing C++ syntax.
Role order must match ev::Role in components/sunsim_events/src/types.cppm
(a different role count fails to compile where it's used).
"""
import json
import sys

ROLE_ORDER = ["WallA", "WallB", "WallC", "Work", "Button"]

HEADER_TEMPLATE = """#pragma once

#include <array>
#include <cstdint>

namespace sunsim {{

// Generated from role_manifest.json by gen_role_manifest.py - do not edit by
// hand. Edit role_manifest.json instead (one IEEE/EUI-64 address per role,
// from the device's label) and rebuild. 0 = no entry. Looked up by
// core::DeviceRegistry when a device joins with no `pair` armed.
inline constexpr std::array<uint64_t, {count}> kRoleManifest = {{{{
{entries}
}}}};

}} // namespace sunsim
"""


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <role_manifest.json> <output role_manifest.h>", file=sys.stderr)
        return 1

    with open(sys.argv[1], encoding="utf-8") as f:
        manifest = json.load(f)

    unknown = set(manifest) - set(ROLE_ORDER)
    if unknown:
        print(f"error: unknown role(s) in {sys.argv[1]}: {sorted(unknown)}", file=sys.stderr)
        return 1

    entries = []
    for role in ROLE_ORDER:
        raw = manifest.get(role, "0x0")
        try:
            value = int(raw, 16)
        except (TypeError, ValueError):
            print(f"error: role '{role}' has non-hex address: {raw!r}", file=sys.stderr)
            return 1
        entries.append(f"    0x{value:016x}, // {role}")

    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write(HEADER_TEMPLATE.format(count=len(ROLE_ORDER), entries="\n".join(entries)))

    return 0


if __name__ == "__main__":
    sys.exit(main())
