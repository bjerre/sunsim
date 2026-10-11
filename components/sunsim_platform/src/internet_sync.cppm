module;

#include <time.h> // localtime_r (POSIX)

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

export module sunsim.platform:internet_sync;

import std.compat;
import sunsim.ev;
import :wifi;

export namespace sunsim::platform {

// Everything that needs the internet, done in one short Wi-Fi session (Wi-Fi
// shares the 2.4 GHz radio with Zigbee): the clock over SNTP, and more jobs
// to come (the day's cloud cover).
//
// When: at boot (blocking, before the Zigbee radio starts); then once a day
// in the sync window (ProgramConfig, `config syncwindow`, default
// 04:00-05:00), retrying every 10 minutes within it on failure; and when
// asked (SyncRequested: console `sync`, switching fast-day mode). While the
// clock has never been set it retries every 10 minutes regardless: the
// lights stay off without it.
class InternetSync {
public:
    InternetSync() = default;

    InternetSync(const InternetSync &)            = delete;
    InternetSync &operator=(const InternetSync &) = delete;

    void syncNow(); // blocking: the boot sync
    void start();   // the background task

    void onConfig(const ev::ConfigChanged &e);
    void onSyncRequested(const ev::SyncRequested &e);

private:
    void run();
    [[nodiscard]] bool due(std::chrono::sys_seconds now) const;
    static void taskTrampoline(void *arg);

    TaskHandle_t task_ = nullptr;

    // Set from the app task, read by the sync task: minutes after midnight.
    std::atomic<uint16_t> window_start_{4 * 60};
    std::atomic<uint16_t> window_end_{5 * 60};

    // The sync task's own (and the boot sync's, before it starts).
    std::optional<std::chrono::sys_seconds> last_try_;
    std::optional<std::chrono::sys_seconds> last_ok_; // every job succeeded
};

} // namespace sunsim::platform

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "INTERNET";

using namespace std::chrono_literals;

constexpr auto kRetryDelay = 10min;

// An unsynced clock starts at 1970; a synced one is well past this.
constexpr std::chrono::sys_days kClockSetAfter = std::chrono::year{2024} / 1 / 1;

std::chrono::sys_seconds now() noexcept
{
    return std::chrono::time_point_cast<std::chrono::seconds>(std::chrono::system_clock::now());
}

// ---- The jobs, in order over one connection ----

bool syncClock()
{
    // Long: while Zigbee shares the radio (busiest in fast-day mode, with
    // light commands every second) requests get lost. lwIP resends after
    // 15, 30, then 60 s, so this allows four tries.
    constexpr auto kTimeout = 2min;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    const bool synced = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(std::chrono::milliseconds{kTimeout}.count())) == ESP_OK;
    esp_netif_sntp_deinit();
    if (synced) {
        const std::time_t t = std::time(nullptr);
        ESP_LOGI(kTag, "Clock synced: %s", std::ctime(&t));
    }
    return synced;
}

struct Job {
    const char *name;
    bool (*run)();
};

// Next: the day's cloud cover (a job here that posts what it fetched as an
// event, as the clock's result shows up in every Tick).
constexpr std::array kJobs{
    Job{"clock", &syncClock},
};

uint16_t minutesOf(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}
} // namespace

void InternetSync::syncNow()
{
    run();
}

void InternetSync::start()
{
    xTaskCreate(&InternetSync::taskTrampoline, "internet", 4096, this, 3, &task_);
}

void InternetSync::onConfig(const ev::ConfigChanged &e)
{
    window_start_ = minutesOf(e.config.sync_start_hour);
    window_end_   = minutesOf(e.config.sync_end_hour);
}

void InternetSync::onSyncRequested(const ev::SyncRequested &)
{
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

void InternetSync::run()
{
    ESP_LOGI(kTag, "Syncing");
    bool ok = false;
    {
        const WifiSession wifi;
        if (wifi.connected()) {
            ok = true;
            for (const Job &job : kJobs) {
                const bool done = job.run();
                ESP_LOGI(kTag, "  %s: %s", job.name, done ? "ok" : "failed");
                ok = ok && done;
            }
        }
    } // Wi-Fi off before anything else
    // Read after the jobs: the clock sync itself moves the clock.
    last_try_ = now();
    if (ok) {
        last_ok_ = last_try_;
    }
    ESP_LOGI(kTag, "%s", ok ? "Sync done" : "Sync failed; will retry");
}

bool InternetSync::due(std::chrono::sys_seconds t) const
{
    const bool retry_ready = !last_try_ || t - *last_try_ >= kRetryDelay;
    if (t < kClockSetAfter) {
        return retry_ready; // never set: keep trying, window or not
    }

    const std::time_t c = std::chrono::system_clock::to_time_t(t);
    std::tm           local{};
    localtime_r(&c, &local);
    const auto minute = std::chrono::minutes{local.tm_hour * 60 + local.tm_min};
    const auto start  = std::chrono::minutes{window_start_.load()};
    if (minute < start || minute >= std::chrono::minutes{window_end_.load()}) {
        return false;
    }
    // When today's window opened: done since then, nothing to do; else the
    // first try in it, or a retry.
    const auto opened = t - (minute - start) - std::chrono::seconds{local.tm_sec};
    if (last_ok_ && *last_ok_ >= opened) {
        return false;
    }
    return !last_try_ || *last_try_ < opened || retry_ready;
}

void InternetSync::taskTrampoline(void *arg)
{
    auto &self = *static_cast<InternetSync *>(arg);
    for (;;) {
        // A minute at a time (a day in ticks would overflow pdMS_TO_TICKS),
        // or at once when asked.
        const bool asked = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60'000)) > 0;
        if (asked || self.due(now())) {
            self.run();
        }
    }
}

} // namespace sunsim::platform
