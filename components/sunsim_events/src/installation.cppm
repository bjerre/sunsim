// The room, as data: what each light shows and when the lights may be on.
// Edit, build, flash. The words are :topology's.

export module sunsim.ev:installation;

import std.compat;
import :topology;
import :types;

export namespace sunsim::ev {

// What each light shows: every light exactly one look.
inline constexpr std::array kShows{
    Shows{"Wall", SunCurve{}},
    Shows{"Work", WorkCurve{}},
};

// When the lights may be on: the active hours (`config hours`, `days`, `fade`).
inline constexpr Gate kGate{
    Window{.hours = &ProgramConfig::active, .days = &ProgramConfig::active_days, .fade = &ProgramConfig::fade},
};

// kShows, per light; checked when it compiles.
inline constexpr PerRole<Look> kLookOf = looksOf(kShows);

} // namespace sunsim::ev
