// The room, as data: which device is which, what each light shows, and when
// the lights may be on. Edit, build, flash. The words are :topology's.

export module sunsim.ev:installation;

import std.compat;
import :topology;
import :types;

export namespace sunsim::ev {

// Each device's IEEE (EUI-64) address, from its label: when it joins, it
// takes that role. A role left at 0 is claimed with `pair <role>` instead.
// (`./sunsim monitor` logs an unknown device's address as it joins.)
inline constexpr List<Address, kRoleCount> kAddresses{
    {"WallA", 0x0000000000000000},
    {"WallB", 0x0000000000000000},
    {"WallC", 0x0000000000000000},
    {"WallD", 0x0000000000000000},
    {"WallE", 0x0000000000000000},
    {"Work", 0x0000000000000000},
    {"Button", 0x0000000000000000},
};

// What each light shows: every light exactly one look.
inline constexpr std::array kShows{
    Shows{"Wall", SunCurve{}},
    Shows{"Work", WorkCurve{}},
};

// When the lights may be on: the active hours (`config hours`, `days`, `fade`).
inline constexpr Gate kGate{
    Window{.hours = &ProgramConfig::active, .days = &ProgramConfig::active_days, .fade = &ProgramConfig::fade},
};

// The tables above, per role; checked when they compile.
inline constexpr Addresses     kAddressOf = addressesOf(kAddresses);
inline constexpr PerRole<Look> kLookOf    = looksOf(kShows);

} // namespace sunsim::ev
