// The installation's groups and device kinds. Role slots and names in
// :types are derived from this table.

export module sunsim.ev:layout;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif

export namespace sunsim::ev {

// What a device does. Each group can hold any number of devices.
enum class Group : uint8_t { Wall, Work, Desk, Grow, Button };

// What a device is, and so what it can do (caps()).
enum class Kind : uint8_t {
    ColorBulb, // on/off, level, color (temperature, xy and hue/saturation)
    Plug,      // on/off, for whatever's plugged into it
    Button,    // presses: its On/Off client, bound to us
};

// What a device takes from us. A light takes at least On/Off; a button
// takes nothing (it sends).
struct Caps {
    bool power = false; // On/Off
    bool level = false; // Level Control
    bool color = false; // Color Control
};

[[nodiscard]] constexpr Caps caps(Kind kind) noexcept
{
    switch (kind) {
    case Kind::ColorBulb:
        return {.power = true, .level = true, .color = true};
    case Kind::Plug:
        return {.power = true};
    case Kind::Button:
        break;
    }
    return {};
}

// N devices of one kind with one job.
struct GroupSpec {
    Group            group;
    std::string_view name;
    Kind             kind;
    uint8_t          count;
};

// The installation: what each group is and how many it has, in Group
// order. A group's members are lettered (WallA, WallB, ...); a group of one
// is just its name (Work, Desk, Grow, Button). Wall lights are lettered east
// to west: their order along the wall drives the sun-direction dimming.
inline constexpr std::array kLayout{
    GroupSpec{Group::Wall, "Wall", Kind::ColorBulb, 5},
    GroupSpec{Group::Work, "Work", Kind::ColorBulb, 1},
    GroupSpec{Group::Desk, "Desk", Kind::Plug, 1},
    GroupSpec{Group::Grow, "Grow", Kind::Plug, 1},
    GroupSpec{Group::Button, "Button", Kind::Button, 1},
};

} // namespace sunsim::ev
