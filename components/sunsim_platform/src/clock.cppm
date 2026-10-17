module;

#include "esp_check.h"
#include "esp_timer.h"
#include "sdkconfig.h"

export module sunsim.platform:clock;

import std.compat;
import sunsim.ev;
import :fast_day_clock;
import :zone;

export namespace sunsim::platform {

// Time for the whole app. A 1 Hz esp_timer posts Heartbeat; on(const ev::Heartbeat &)
// (app task) reads the clock and posts Tick, plus ProgramTick every update
// interval: 60 s, or 1 s in the debug fast-day mode, whose synthetic clock
// loops the configured active window. Fast-day is switched at runtime
// (SetFastDay, console `fastday`; menuconfig picks how it boots). The real
// clock is kept set by InternetSync.
class Clock {
public:
    explicit Clock(ev::Post post);
    ~Clock();

    Clock(const Clock &)            = delete;
    Clock &operator=(const Clock &) = delete;

    void start();

    void on(const ev::Heartbeat &);
    void on(const ev::ConfigChanged &e);
    void on(const ev::SetFastDay &e);

private:
    [[nodiscard]] ev::TimeReading read() const;
    static void heartbeatTrampoline(void *arg);

    ev::Post                      post_;
    esp_timer_handle_t            timer_ = nullptr;
    ev::ProgramConfig             config_{};
    std::optional<FastDayClock>   fast_day_; // in fast-day mode
    std::chrono::seconds          update_interval_{60}; // 1 s in fast-day mode
    std::chrono::seconds          since_update_{};
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "CLOCK";

constexpr auto kUpdateInterval        = std::chrono::seconds{60};
constexpr auto kFastDayUpdateInterval = std::chrono::seconds{1}; // the gradient visibly animates

// SNTP-synced time is always well past this; an unsynced clock starts at 1970.
constexpr std::chrono::year kMinValidYear{2024};

} // namespace

Clock::Clock(ev::Post post) : post_(std::move(post))
{
#if CONFIG_SUNSIM_DEBUG_FAST_DAY
    fast_day_.emplace(std::chrono::seconds{CONFIG_SUNSIM_DEBUG_DAY_SECONDS});
    update_interval_ = kFastDayUpdateInterval;
    ev::log::warn(kTag, "DEBUG FAST DAY MODE: one active window every {} s (`fastday off` for real time)",
                  CONFIG_SUNSIM_DEBUG_DAY_SECONDS);
#endif
}

Clock::~Clock() = default;

void Clock::start()
{
    const esp_timer_create_args_t args{
        .callback              = &Clock::heartbeatTrampoline,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "heartbeat",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1'000'000));
}

ev::TimeReading Clock::read() const
{
    using namespace std::chrono;
    const auto      now = floor<seconds>(system_clock::now());
    ev::TimeReading t{.utc_offset = utcOffset(now)};
    if (fast_day_) {
        t.local     = fast_day_->now(config_);
        t.valid     = true;
        t.simulated = true;
    } else {
        t.local = local_seconds{now.time_since_epoch()} + t.utc_offset;
        t.valid = year_month_day{t.day()}.year() >= kMinValidYear;
    }
    return t;
}

void Clock::on(const ev::Heartbeat &)
{
    const ev::TimeReading time = read();
    post_(ev::Tick{.at = ev::BootClock::now(), .time = time});

    if (since_update_ += std::chrono::seconds{1}; since_update_ >= update_interval_) {
        since_update_ = {};
        // Ramp over the whole interval, so bulbs fade continuously from one
        // update to the next instead of jumping and holding.
        post_(ev::ProgramTick{.time = time, .transition = update_interval_});
    }
}

void Clock::on(const ev::ConfigChanged &e)
{
    config_ = e.config;
}

void Clock::on(const ev::SetFastDay &e)
{
    const bool was_on = fast_day_.has_value();
    if (e.on) {
        fast_day_.emplace(e.day_length); // (re)starts the loop
        update_interval_ = kFastDayUpdateInterval;
        ev::log::warn(kTag, "Fast-day mode: one active window every {} s", static_cast<long long>(e.day_length.count()));
    } else if (was_on) {
        fast_day_.reset();
        update_interval_ = kUpdateInterval;
        ev::log::info(kTag, "Fast-day mode off: real time");
    } else {
        return; // already off
    }
    since_update_ = {};

    // Show the new time now, with the quick transition, rather than at the
    // next update: programs refresh from the latest Tick's time.
    post_(ev::Tick{.at = ev::BootClock::now(), .time = read()});
    post_(ev::RefreshLights{});
    if (e.on != was_on) {
        post_(ev::SyncRequested{}); // switching modes: a moment to sync and fetch
    }
}

void Clock::heartbeatTrampoline(void *arg)
{
    auto *self = static_cast<Clock *>(arg);
    self->post_(ev::Heartbeat{});
}

} // namespace sunsim::platform
