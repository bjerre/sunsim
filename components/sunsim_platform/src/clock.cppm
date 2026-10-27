module;

#include "esp_timer.h" // esp_timer_handle_t

export module sunsim.platform:clock;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :fast_day_clock;

export namespace sunsim::platform {

// Time for the whole app. A 1 Hz esp_timer posts Heartbeat, stamped when it
// fired; on(const ev::Heartbeat &) (app task) reads the clock and posts Tick
// at that time, plus ProgramTick every update interval: 60 s, or 1 s in the
// debug fast-day mode, whose synthetic clock loops the configured active
// window. Fast-day is switched at runtime (SetFastDay, console `fastday`;
// menuconfig picks how it boots). The real clock is kept set by
// InternetSync. A WakeAt arms a one-shot esp_timer that posts Wake then, for
// flows that can't wait for the next Tick. (Defined in clock.cpp, a module
// implementation unit: defined here, GCC 15 crashes writing the module
// interface; see ARCHITECTURE.md §12.)
class Clock {
public:
    explicit Clock(ev::Post post);
    ~Clock();

    Clock(const Clock &)            = delete;
    Clock &operator=(const Clock &) = delete;

    void start();

    void on(const ev::Heartbeat &e);
    void on(const ev::WakeAt &e);
    void on(const ev::Wake &e);
    void on(const ev::ConfigChanged &e);
    void on(const ev::SetFastDay &e);

private:
    [[nodiscard]] ev::TimeReading read() const;
    void        armWake();
    static void heartbeatTrampoline(void *arg);
    static void wakeTrampoline(void *arg);

    ev::Post                    post_;
    esp_timer_handle_t          timer_      = nullptr;
    esp_timer_handle_t          wake_timer_ = nullptr;
    ev::ProgramConfig           config_{};
    std::optional<FastDayClock> fast_day_;            // in fast-day mode
    std::chrono::seconds        update_interval_{60}; // 1 s in fast-day mode
    std::optional<ev::Instant>  next_update_;         // the next ProgramTick; none until a Heartbeat sets it
    std::vector<ev::Instant>    wakes_;               // asked for (WakeAt), soonest first
};

} // namespace sunsim::platform
