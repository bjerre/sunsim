module;

#include <time.h> // localtime_r (POSIX)

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

// The local civil UTC offset (including DST) at `at`, whose local time is
// `local`: the local calendar fields read as if they were UTC, less the
// instant itself. (Without a portable tm_gmtoff. mktime() on gmtime()'s
// fields would be off by the DST hour: gmtime() sets tm_isdst = 0.)
double utcOffsetHours(std::time_t at, const std::tm &local) noexcept
{
    using namespace std::chrono;
    const sys_seconds as_if_utc = sys_days{year{local.tm_year + 1900} / (local.tm_mon + 1) / local.tm_mday} +
                                  hours{local.tm_hour} + minutes{local.tm_min} + seconds{local.tm_sec};
    return duration<double, std::ratio<3600>>{as_if_utc - sys_seconds{seconds{at}}}.count();
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
    const auto now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm    local{};
    localtime_r(&now_c, &local);

    ev::TimeReading t;
    t.utc_offset_hours = utcOffsetHours(now_c, local);
    if (fast_day_) {
        t.local     = fast_day_->now(config_);
        t.valid     = true;
        t.simulated = true;
    } else {
        t.local = local;
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
