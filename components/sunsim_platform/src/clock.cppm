module;

#include <time.h> // gmtime_r, localtime_r (POSIX)

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

export module sunsim.platform:clock;

import std.compat;
import sunsim.ev;
import :fast_day_clock;
import :wifi_time_sync;

export namespace sunsim::platform {

// Time for the whole app. A 1 Hz esp_timer posts Heartbeat; onHeartbeat()
// (app task) reads the clock and posts Tick, plus ProgramTick every update
// interval (60 s; 1 s in the debug fast-day mode, whose synthetic clock
// follows the configured active window). Also keeps the real clock set:
// SNTP over Wi-Fi at boot, then retries/daily resyncs on a background task.
class Clock {
public:
    explicit Clock(ev::Post post);
    ~Clock();

    Clock(const Clock &)            = delete;
    Clock &operator=(const Clock &) = delete;

    // Blocking first sync (call before the Zigbee stack starts, so the radio
    // isn't shared at boot), then the background retry/resync task.
    // No-op in fast-day mode.
    void startTimeSync();
    void start();

    void onHeartbeat();
    void onConfig(const ev::ConfigChanged &e);

private:
    [[nodiscard]] ev::TimeReading read() const;
    static void heartbeatTrampoline(void *arg);
    static void timeSyncTask(void *arg);

    ev::Post                      post_;
    esp_timer_handle_t            timer_ = nullptr;
    ev::ProgramConfig             config_{};
    std::unique_ptr<FastDayClock> fast_day_; // set in fast-day mode
    std::chrono::seconds          update_interval_{60};
    std::chrono::seconds          since_update_{};
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "CLOCK";

// Time sync: retry soon after a failure (router still booting after a power
// cut, Wi-Fi not configured yet), otherwise resync daily to cancel drift.
constexpr auto kSyncRetryDelay = std::chrono::minutes{10};
constexpr auto kResyncInterval = std::chrono::hours{24};

// SNTP-synced time is always well past this; an unsynced clock starts at 1970.
constexpr int kMinValidYear = 2024;

// The current local civil UTC offset (including DST), derived without a
// portable tm_gmtoff: reinterpreting today's UTC calendar fields as if they
// were local time tells mktime() how far local time has drifted from UTC
// under the process's TZ rules.
double currentUtcOffsetHours() noexcept
{
    const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    utc_tm{};
    gmtime_r(&now_c, &utc_tm);
    const std::time_t utc_as_if_local = std::mktime(&utc_tm);
    return std::difftime(now_c, utc_as_if_local) / 3600.0;
}
} // namespace

Clock::Clock(ev::Post post) : post_(std::move(post))
{
#if CONFIG_SUNSIM_DEBUG_FAST_DAY
    fast_day_          = std::make_unique<FastDayClock>(std::chrono::seconds{CONFIG_SUNSIM_DEBUG_DAY_SECONDS});
    update_interval_   = std::chrono::seconds{1};
    ESP_LOGW(kTag, "DEBUG FAST DAY MODE: simulating one active window (work day) every %d seconds",
             CONFIG_SUNSIM_DEBUG_DAY_SECONDS);
#endif
}

Clock::~Clock() = default;

void Clock::startTimeSync()
{
    // Compiled out (not just skipped) in fast-day mode, so the Wi-Fi stack
    // isn't linked into a build that never uses it.
#if !CONFIG_SUNSIM_DEBUG_FAST_DAY
    const bool synced = WifiTimeSync{}.run();
    xTaskCreate(&Clock::timeSyncTask, "time_sync", 4096, reinterpret_cast<void *>(synced ? 1 : 0), 3, nullptr);
#endif
}

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
    ev::TimeReading t;
    t.utc_offset_hours = currentUtcOffsetHours();
    if (fast_day_) {
        t.local     = fast_day_->now(config_);
        t.valid     = true;
        t.simulated = true;
    } else {
        const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        localtime_r(&now_c, &t.local);
        t.valid = t.local.tm_year + 1900 >= kMinValidYear;
    }
    return t;
}

void Clock::onHeartbeat()
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

void Clock::onConfig(const ev::ConfigChanged &e)
{
    config_ = e.config;
}

void Clock::heartbeatTrampoline(void *arg)
{
    auto *self = static_cast<Clock *>(arg);
    self->post_(ev::Heartbeat{});
}

void Clock::timeSyncTask([[maybe_unused]] void *arg)
{
#if !CONFIG_SUNSIM_DEBUG_FAST_DAY
    bool synced = arg != nullptr;
    for (;;) {
        const auto wait = synced ? std::chrono::duration_cast<std::chrono::minutes>(kResyncInterval) : kSyncRetryDelay;
        // One minute at a time: a full day in ticks can overflow pdMS_TO_TICKS.
        for (auto m = wait.count(); m > 0; --m) {
            vTaskDelay(pdMS_TO_TICKS(60'000));
        }
        synced = WifiTimeSync{}.run();
    }
#endif
}

} // namespace sunsim::platform
