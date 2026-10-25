export module sunsim.core:program_host;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :daylight_program;
import :off_program;
import :program;
import :red_alert_program;
import :tuning_program;

export namespace sunsim::core {

// Owns the lighting programs, a set per zone, and runs the one each zone's
// Mode selects: it starts it on the zone's ModeChanged, and passes it the
// ticks and refreshes. Settings, the weather and the console's tuning go to
// every program that handles them (see Program). Keeps the latest time, for
// programs that need it when they start or refresh. On `tune save` it posts
// the levels tuned in the zones that are tuning as the lights' minimums.
//
// Adding a program: a Mode (in sunsim::ev: the enum, kModes, modeName), a
// Program subclass, its place in Programs here, and whatever selects it
// (a zone's Controls, the console).
class ProgramHost {
public:
    explicit ProgramHost(const ev::Post &post);

    void on(const ev::ModeChanged &e);
    void on(const ev::Tick &e);
    void on(const ev::ProgramTick &e);
    void on(const ev::RefreshLights &e);
    void on(const ev::NetworkUp &) { on(ev::RefreshLights{}); }
    void on(const ev::ConfigChanged &e);
    void on(const ev::SkyChanged &e) { toEvery(e); }
    void on(const ev::TuneLights &e) { toEvery(e); }
    void on(const ev::TuneSave &e);

private:
    // One of each, in Mode order: element m is mode m's program.
    using Programs = std::tuple<DaylightProgram, OffProgram, RedAlertProgram, TuningProgram>;

    static constexpr bool inModeOrder = []<std::size_t... I>(std::index_sequence<I...>) {
        return ((LightingProgram<std::tuple_element_t<I, Programs>> &&
                 std::to_underlying(std::tuple_element_t<I, Programs>::kMode) == I) &&
                ...);
    }(std::make_index_sequence<std::tuple_size_v<Programs>>{});
    static_assert(std::tuple_size_v<Programs> == ev::kModes.size() && inModeOrder,
                  "Programs holds one program per Mode, in Mode order");

    // A zone's programs, and the mode it's in.
    struct Zone {
        Zone(const ev::Post &post, ev::ZoneId id)
            : programs{[&]<std::size_t... I>(std::index_sequence<I...>) {
                  return Programs{(void(I), Program::Context{post, id})...};
              }(std::make_index_sequence<std::tuple_size_v<Programs>>{})}
        {
        }

        // f(the running program)
        template <class F> void running(F &&f)
        {
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                ((std::to_underlying(mode) == I ? f(std::get<I>(programs)) : void()), ...);
            }(std::make_index_sequence<std::tuple_size_v<Programs>>{});
        }

        Programs programs;
        ev::Mode mode = ev::Mode::Daylight;
    };

    // e to every program that handles it, or to each zone's running one if it does.
    template <class E> void toEvery(const E &e)
    {
        for (Zone &zone : zones_) {
            std::apply([&](auto &...p) { (deliver(p, e), ...); }, zone.programs);
        }
    }
    template <class E> void toRunning(const E &e)
    {
        for (Zone &zone : zones_) {
            zone.running([&](auto &p) { deliver(p, e); });
        }
    }
    template <class P, class E> static void deliver(P &program, const E &e)
    {
        if constexpr (ev::HandlerOf<P, E>) {
            program.on(e);
        }
    }

    std::array<Zone, ev::kZoneCount> zones_;
    ev::Post                         post_;
    std::optional<ev::TimeReading>   time_; // latest, from Tick/ProgramTick
};

} // namespace sunsim::core

namespace sunsim::core {

ProgramHost::ProgramHost(const ev::Post &post)
    : zones_{[&]<std::size_t... Z>(std::index_sequence<Z...>) {
          return std::array<Zone, ev::kZoneCount>{Zone{post, ev::kZoneIds[Z]}...};
      }(std::make_index_sequence<ev::kZoneCount>{})},
      post_(post)
{
}

void ProgramHost::on(const ev::ModeChanged &e)
{
    Zone &zone = zones_[e.zone.index];
    zone.mode  = e.mode;
    zone.running([&](auto &p) {
        if constexpr (requires { p.start(time_); }) {
            p.start(time_);
        } else {
            p.refresh(std::nullopt, time_);
        }
    });
}

void ProgramHost::on(const ev::Tick &e)
{
    time_ = e.time;
    toRunning(e);
}

void ProgramHost::on(const ev::ProgramTick &e)
{
    time_ = e.time;
    toRunning(e);
}

void ProgramHost::on(const ev::RefreshLights &e)
{
    for (Zone &zone : zones_) {
        zone.running([&](auto &p) { p.refresh(e.role, time_); }); // a program skips lights not in its zone
    }
}

void ProgramHost::on(const ev::ConfigChanged &e)
{
    toEvery(e);
    on(ev::RefreshLights{}); // show a change now, not at the next update
}

void ProgramHost::on(const ev::TuneSave &)
{
    for (Zone &zone : zones_) {
        const auto tuned = std::get<TuningProgram>(zone.programs).takeTuned();
        if (zone.mode != ev::Mode::Tuning) {
            continue; // `tune` reached it, but it isn't tuning: nothing it saw counts
        }
        for (const ev::Role role : ev::lights()) {
            if (tuned[role]) {
                post_(ev::SetMinLevel{.lights = role, .level = *tuned[role]});
            }
        }
    }
}

} // namespace sunsim::core
