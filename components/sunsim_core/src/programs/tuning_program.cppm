export module sunsim.core:tuning_program;

import std.compat;
import sunsim.ev;
import :daylight;
import :program;

export namespace sunsim::core {

// For finding the lowest level each bulb visibly shows (bulbs differ, and
// below it they go dark before sunset). Holds every bulb steady: at its
// minimum level, in the sunset's amber, until the console (`tune`) sets a
// level or colour. On `tune save`, ProgramHost takes the tuned levels and
// makes them the bulbs' minimums (SetMinLevel), which the daylight curve
// then never goes below while on.
class TuningProgram final : public Program {
public:
    static constexpr ev::Mode kMode = ev::Mode::Tuning;
    using Program::Program;

    void start(const std::optional<ev::TimeReading> &now);
    void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now);
    void on(const ev::ConfigChanged &e);

    void on(const ev::TuneLights &e);

    // The levels set with `tune` since the last take, and forgets them.
    [[nodiscard]] ev::PerRole<std::optional<uint8_t>> takeTuned() noexcept { return std::exchange(tuned_, {}); }

private:
    [[nodiscard]] uint8_t level(ev::Role role) const noexcept { return tuned_[role].value_or(min_levels_[role]); }
    void                  emit(ev::Role role) const;

    ev::MinLevels                        min_levels_ = ev::kDefaultMinLevels;
    ev::PerRole<std::optional<uint8_t>>  tuned_{};  // set with `tune`, not yet saved
    double                               kelvin_ = 1800.0; // amber_kelvin until `tune` gives one
};

} // namespace sunsim::core

namespace sunsim::core {

void TuningProgram::start(const std::optional<ev::TimeReading> &now)
{
    tuned_ = {};
    refresh(std::nullopt, now);
}

void TuningProgram::refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &)
{
    forEachBulb(role, [&](ev::Role r) { emit(r); });
}

void TuningProgram::on(const ev::ConfigChanged &e)
{
    min_levels_ = e.min_levels;
    kelvin_     = e.config.amber_kelvin;
}

void TuningProgram::on(const ev::TuneLights &e)
{
    if (e.kelvin) {
        kelvin_ = *e.kelvin;
    }
    forEachBulb(std::nullopt, [&](ev::Role r) {
        if (ev::selects(e.lights, r)) {
            tuned_[r] = std::clamp<uint8_t>(e.level, 1, ev::kMaxLevel);
        }
    });
    refresh(std::nullopt, std::nullopt); // a colour change applies to every bulb
}

void TuningProgram::emit(ev::Role role) const
{
    // Like the sunset, where bulbs go dark first: xy below ~2700K.
    ev::LightState state{.on = true, .level = level(role), .mireds = static_cast<uint16_t>(std::lround(1e6 / kelvin_))};
    if (kelvin_ < 2700.0) {
        state.color = ev::ColorMode::Xy;
        planckianXy(kelvin_, state.color_x, state.color_y);
    }
    target(role, state, std::chrono::seconds{1});
}

} // namespace sunsim::core
