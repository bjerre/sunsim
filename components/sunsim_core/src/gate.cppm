export module sunsim.core:gate;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// How open `gate` is at `time`: 0 (the lights off) to 1, in between while
// fading. The first rule with an answer decides; when none answers, 0.
[[nodiscard]] double openness(const ev::Gate &gate, const ev::TimeReading &time,
                              const ev::ProgramConfig &config) noexcept;

} // namespace sunsim::core

namespace sunsim::core {

namespace {

// One rule's answer, before Says: how open, or nullopt for no answer.
using Answer = std::optional<double>;

Answer answer(const ev::Always &, const ev::TimeReading &, const ev::ProgramConfig &) noexcept
{
    return 1.0;
}

Answer answer(const ev::Window &window, const ev::TimeReading &time, const ev::ProgramConfig &config) noexcept
{
    const ev::DaySpan          hours = window.hours(config);
    const std::chrono::minutes fade  = window.fade(config);
    const std::chrono::seconds now   = time.sinceMidnight();
    if (!window.days(config).contains(time.weekday()) || !hours.contains(now)) {
        return 0.0;
    }
    if (fade <= std::chrono::minutes{0}) {
        return 1.0;
    }
    // How far into the fade at either edge, 0 to 1 and beyond.
    const auto fade_in  = std::chrono::duration<double>{now - hours.start} / fade;
    const auto fade_out = std::chrono::duration<double>{hours.end - now} / fade;
    return std::fmin(1.0, std::fmin(fade_in, fade_out));
}

// What `rule` says: its answer, unless it's the case it leaves to the rules after it.
Answer says(const ev::Rule &rule, const ev::TimeReading &time, const ev::ProgramConfig &config) noexcept
{
    const Answer a = std::visit([&](const auto &source) { return answer(source, time, config); }, rule.source);
    switch (rule.says) {
    case ev::Says::OnOnly:
        return a > 0.0 ? a : std::nullopt;
    case ev::Says::OffOnly:
        return a && *a <= 0.0 ? a : std::nullopt;
    case ev::Says::Both:
        break;
    }
    return a;
}

} // namespace

double openness(const ev::Gate &gate, const ev::TimeReading &time, const ev::ProgramConfig &config) noexcept
{
    for (const ev::Rule &rule : gate) {
        if (const Answer a = says(rule, time, config)) {
            return *a;
        }
    }
    return 0.0;
}

} // namespace sunsim::core
