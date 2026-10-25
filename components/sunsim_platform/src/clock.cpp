module;

#include "esp_check.h"
#include "esp_timer.h"
#include "sdkconfig.h"

module sunsim.platform;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :fast_day_clock;
import :zone;

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
    const esp_timer_create_args_t heartbeat{
        .callback              = &Clock::heartbeatTrampoline,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "heartbeat",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&heartbeat, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1'000'000));

    const esp_timer_create_args_t wake{
        .callback              = &Clock::wakeTrampoline,
        .arg                   = this,
        .dispatch_method       = ESP_TIMER_TASK,
        .name                  = "wake",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&wake, &wake_timer_));
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

void Clock::on(const ev::Heartbeat &e)
{
    const ev::TimeReading time = read();
    post_(ev::Tick{.at = e.at, .time = time});

    if (!next_update_) {
        next_update_ = e.at + update_interval_;
    } else if (e.at >= *next_update_) {
        // On schedule rather than drifting by how late each Heartbeat was
        // handled; after a long stall, from now.
        *next_update_ += update_interval_;
        if (*next_update_ <= e.at) {
            next_update_ = e.at + update_interval_;
        }
        // Ramp over the whole interval, so bulbs fade continuously from one
        // update to the next instead of jumping and holding.
        post_(ev::ProgramTick{.time = time, .transition = update_interval_});
    }
}

void Clock::on(const ev::WakeAt &e)
{
    const auto at = std::ranges::lower_bound(wakes_, e.at);
    if (at != wakes_.end() && *at == e.at) {
        return; // already asked for
    }
    wakes_.insert(at, e.at);
    armWake();
}

void Clock::on(const ev::Wake &e)
{
    std::erase_if(wakes_, [&](ev::Instant w) { return w <= e.at; });
    armWake();
}

// The one-shot timer, for the soonest wake asked for.
void Clock::armWake()
{
    esp_timer_stop(wake_timer_); // fails, harmlessly, if it isn't running
    if (wakes_.empty()) {
        return;
    }
    const auto in = std::chrono::duration_cast<std::chrono::microseconds>(wakes_.front() - ev::BootClock::now());
    ESP_ERROR_CHECK(esp_timer_start_once(wake_timer_, std::max<int64_t>(1, in.count())));
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
    next_update_.reset(); // the new interval, from the next Heartbeat

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
    static_cast<Clock *>(arg)->post_(ev::Heartbeat{.at = ev::BootClock::now()});
}

void Clock::wakeTrampoline(void *arg)
{
    static_cast<Clock *>(arg)->post_(ev::Wake{.at = ev::BootClock::now()});
}

} // namespace sunsim::platform
