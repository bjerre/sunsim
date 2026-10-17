export module sunsim.core:program_host;

import std.compat;
import sunsim.ev;
import :daylight_program;
import :off_program;
import :program;
import :red_alert_program;
import :tuning_program;

export namespace sunsim::core {

// Owns the lighting programs and runs the one the Mode selects: it starts
// it on ModeChanged, and passes it the ticks and refreshes. Settings, the
// weather and the console's tuning go to every program that handles them
// (see Program). Keeps the latest time, for programs that need it when they
// start or refresh. On `tune save` it posts the tuning program's levels as
// the lights' minimums.
//
// Adding a program: a Mode (in sunsim::ev: the enum, kModes, modeName), a
// Program subclass, its place in Programs here, and whatever selects it
// (ModeController, console).
class ProgramHost {
public:
    explicit ProgramHost(const ev::Post &post);

    void on(const ev::ModeChanged &e);
    void on(const ev::Tick &e);
    void on(const ev::ProgramTick &e);
    void on(const ev::RefreshLights &e) { running([&](auto &p) { p.refresh(e.role, time_); }); }
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

    // f(the running program)
    template <class F> void running(F &&f)
    {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            ((std::to_underlying(mode_) == I ? f(std::get<I>(programs_)) : void()), ...);
        }(std::make_index_sequence<std::tuple_size_v<Programs>>{});
    }

    // e to every program that handles it, or to the running one if it does.
    template <class E> void toEvery(const E &e)
    {
        std::apply([&](auto &...p) { (deliver(p, e), ...); }, programs_);
    }
    template <class E> void toRunning(const E &e)
    {
        running([&](auto &p) { deliver(p, e); });
    }
    template <class P, class E> static void deliver(P &program, const E &e)
    {
        if constexpr (ev::HandlerOf<P, E>) {
            program.on(e);
        }
    }

    Programs programs_;
    ev::Post post_;

    ev::Mode                       mode_ = ev::Mode::Daylight;
    std::optional<ev::TimeReading> time_; // latest, from Tick/ProgramTick
};

} // namespace sunsim::core

namespace sunsim::core {

ProgramHost::ProgramHost(const ev::Post &post) : programs_{post, post, post, post}, post_(post) {}

void ProgramHost::on(const ev::ModeChanged &e)
{
    mode_ = e.mode;
    running([&](auto &p) {
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

void ProgramHost::on(const ev::ConfigChanged &e)
{
    toEvery(e);
    on(ev::RefreshLights{}); // show a change now, not at the next update
}

void ProgramHost::on(const ev::TuneSave &)
{
    const auto tuned = std::get<TuningProgram>(programs_).takeTuned();
    for (const ev::Role role : ev::lights()) {
        if (tuned[role]) {
            post_(ev::SetMinLevel{.lights = role, .level = *tuned[role]});
        }
    }
}

} // namespace sunsim::core
