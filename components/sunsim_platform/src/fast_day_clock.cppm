module sunsim.platform:fast_day_clock; // internal: not exported

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

namespace sunsim::platform {

// Compresses a day into a short, continuously-looping real-time window - for
// bench-testing the daylight program without waiting for an actual
// sunrise/sunset. Construct once; now() reflects elapsed real time since
// construction.
//
// Only the active window is simulated (ev::ProgramConfig's active hours and
// days, read live on each call so `config` changes show up on the next
// loop): each loop runs through the active hours, and successive loops step
// through the active days only, Monday first, so no loop time is spent on
// hours or days where the lights are off anyway. The dates are those days
// of the first week of 2024 (Monday the 1st): a winter sun.
class FastDayClock {
public:
    explicit FastDayClock(std::chrono::seconds real_day_length) noexcept
        : real_day_(real_day_length), start_(std::chrono::steady_clock::now())
    {
    }

    [[nodiscard]] std::chrono::local_seconds now(const ev::ProgramConfig &config) const noexcept
    {
        using namespace std::chrono;
        const auto   elapsed = steady_clock::now() - start_;
        const double through = duration<double>{elapsed % real_day_} / real_day_; // this loop, 0 to 1
        const auto   hours   = config.active.end - config.active.start;
        const auto   time    = config.active.start + round<seconds>(duration<double>{hours} * through);
        return day(config.active_days, elapsed / real_day_) + time;
    }

private:
    static constexpr std::chrono::local_days kFirstMonday{std::chrono::year{2024} / std::chrono::January / 1};

    // The loop-th active day, cycling Monday first.
    [[nodiscard]] static std::chrono::local_days day(ev::Weekdays active, long long loop) noexcept
    {
        std::vector<std::chrono::local_days> week;
        for (int i = 0; i < 7; ++i) {
            const auto d = kFirstMonday + std::chrono::days{i};
            if (active.contains(std::chrono::weekday{d})) {
                week.push_back(d);
            }
        }
        return week.empty() ? kFirstMonday : week[static_cast<std::size_t>(loop) % week.size()];
    }

    std::chrono::steady_clock::duration   real_day_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace sunsim::platform
