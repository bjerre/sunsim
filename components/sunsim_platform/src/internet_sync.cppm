module;

#include <time.h> // localtime_r (POSIX)

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

export module sunsim.platform:internet_sync;

import std.compat;
import sunsim.ev;
import :wifi;

export namespace sunsim::platform {

// Everything that needs the internet, done in one short Wi-Fi session (Wi-Fi
// shares the 2.4 GHz radio with Zigbee), in order: the clock over SNTP, then
// the hourly cloud cover for today and tomorrow (Open-Meteo, posted as
// CloudCoverUpdated; over HTTPS, which needs the clock).
//
// When: at boot (start() waits for it, before the Zigbee radio starts); then
// once a day in the sync window (ProgramConfig, `config syncwindow`, default
// 04:00-05:00), retrying every 10 minutes within it on failure; and when
// asked (SyncRequested: console `sync`, switching fast-day mode). While the
// clock has never been set it retries every 10 minutes regardless: the
// lights stay off without it. Everything runs on its own task (TLS needs
// the stack).
class InternetSync {
public:
    // The location is menuconfig's: the forecast is for it.
    InternetSync(ev::Post post, double latitude_deg, double longitude_deg);

    InternetSync(const InternetSync &)            = delete;
    InternetSync &operator=(const InternetSync &) = delete;

    // Starts the task and waits for its first sync (the boot sync).
    void start();

    void onConfig(const ev::ConfigChanged &e);
    void onSyncRequested(const ev::SyncRequested &e);

private:
    void run();
    bool syncClock();
    bool fetchCloudCover();
    [[nodiscard]] bool due(std::chrono::sys_seconds now) const;
    static void taskTrampoline(void *arg);

    ev::Post          post_;
    double            latitude_deg_;
    double            longitude_deg_;
    TaskHandle_t      task_      = nullptr;
    SemaphoreHandle_t boot_done_ = nullptr;

    // Set from the app task, read by the sync task: minutes after midnight.
    std::atomic<uint16_t> window_start_{4 * 60};
    std::atomic<uint16_t> window_end_{5 * 60};

    // The sync task's own.
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

uint16_t minutesOf(double hours) noexcept
{
    return static_cast<uint16_t>(std::lround(hours * 60.0));
}

std::tm localTime(std::time_t t) noexcept
{
    std::tm local{};
    localtime_r(&t, &local);
    return local;
}

std::chrono::sys_days dateOf(const std::tm &t) noexcept
{
    return std::chrono::sys_days{std::chrono::year{t.tm_year + 1900} / 1 / 1} + std::chrono::days{t.tm_yday};
}

// The numbers in a JSON array `"key":[...]`, null as nullopt. Enough for
// Open-Meteo's flat reply; not a general parser.
std::vector<std::optional<long long>> jsonNumbers(std::string_view json, std::string_view key)
{
    std::vector<std::optional<long long>> out;
    const std::string needle = "\"" + std::string{key} + "\":[";
    const auto        start  = json.find(needle);
    if (start == std::string_view::npos) {
        return out;
    }
    std::string_view rest = json.substr(start + needle.size());
    rest                  = rest.substr(0, rest.find(']'));
    while (!rest.empty()) {
        const auto       comma = rest.find(',');
        std::string_view item  = rest.substr(0, comma);
        long long        value = 0;
        const auto [end, err]  = std::from_chars(item.data(), item.data() + item.size(), value);
        out.push_back(err == std::errc{} ? std::optional{value} : std::nullopt); // "null"
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    return out;
}

// GET over HTTPS (checked against the certificate bundle): the body, or
// nullopt.
std::optional<std::string> httpsGet(const char *url)
{
    esp_http_client_config_t config{};
    config.url               = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms        = 30'000; // slow while Zigbee shares the radio
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return std::nullopt;
    }
    std::optional<std::string> body;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        const int   status = esp_http_client_get_status_code(client);
        std::string text;
        char        chunk[512];
        int         n = 0;
        while (text.size() < 16 * 1024 && (n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
            text.append(chunk, static_cast<std::size_t>(n));
        }
        if (status == 200) {
            body = std::move(text);
        } else {
            ESP_LOGW(kTag, "HTTP %d from %s", status, url);
        }
    } else {
        ESP_LOGW(kTag, "Could not reach %s", url);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return body;
}
} // namespace

InternetSync::InternetSync(ev::Post post, double latitude_deg, double longitude_deg)
    : post_(std::move(post)), latitude_deg_(latitude_deg), longitude_deg_(longitude_deg)
{
}

void InternetSync::start()
{
    boot_done_ = xSemaphoreCreateBinary();
    xTaskCreate(&InternetSync::taskTrampoline, "internet", 8192, this, 3, &task_);
    xSemaphoreTake(boot_done_, portMAX_DELAY);
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

// ---- The jobs ----

bool InternetSync::syncClock()
{
    // Long: while Zigbee shares the radio (busiest in fast-day mode, with
    // light commands every second) requests get lost. lwIP resends after
    // 15, 30, then 60 s, so this allows four tries.
    constexpr auto kTimeout = 2min;

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);
    const bool synced =
        esp_netif_sntp_sync_wait(pdMS_TO_TICKS(std::chrono::milliseconds{kTimeout}.count())) == ESP_OK;
    esp_netif_sntp_deinit();
    if (synced) {
        const std::time_t t = std::time(nullptr);
        ESP_LOGI(kTag, "Clock synced: %s", std::ctime(&t));
    }
    return synced;
}

// Open-Meteo's hourly cloud cover (free, no key), yesterday through the
// day after tomorrow in UTC so every local hour of today and tomorrow is
// covered whatever the time zone, placed by local hour from today's 00:00.
bool InternetSync::fetchCloudCover()
{
    char url[256];
    std::snprintf(url, sizeof(url),
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&hourly=cloud_cover"
                  "&timeformat=unixtime&timezone=GMT&past_days=1&forecast_days=3",
                  latitude_deg_, longitude_deg_);
    const auto body = httpsGet(url);
    if (!body) {
        return false;
    }
    const auto times  = jsonNumbers(*body, "time");
    const auto covers = jsonNumbers(*body, "cloud_cover");
    if (times.empty() || times.size() != covers.size()) {
        ESP_LOGW(kTag, "Unexpected forecast reply (%u bytes)", static_cast<unsigned>(body->size()));
        return false;
    }

    const std::tm  today = localTime(std::time(nullptr));
    ev::CloudCover forecast;
    forecast.year      = static_cast<int16_t>(today.tm_year + 1900);
    forecast.yday      = static_cast<int16_t>(today.tm_yday);
    std::size_t placed = 0;
    for (std::size_t i = 0; i < times.size(); ++i) {
        if (!times[i] || !covers[i]) {
            continue;
        }
        const std::tm local = localTime(static_cast<std::time_t>(*times[i]));
        const long    slot  = (dateOf(local) - dateOf(today)).count() * 24 + local.tm_hour;
        if (slot >= 0 && slot < static_cast<long>(ev::CloudCover::kHours)) {
            forecast.percent[static_cast<std::size_t>(slot)] =
                static_cast<uint8_t>(std::clamp<long long>(*covers[i], 0, 100));
            ++placed;
        }
    }
    if (placed == 0) {
        return false;
    }
    ESP_LOGI(kTag, "Cloud cover, %u hours; today 08:00 %u%%, 12:00 %u%%, 16:00 %u%%", static_cast<unsigned>(placed),
             forecast.percent[8], forecast.percent[12], forecast.percent[16]);
    post_(ev::CloudCoverUpdated{forecast});
    return true;
}

// ---- Scheduling ----

void InternetSync::run()
{
    struct Job {
        const char *name;
        bool (InternetSync::*run)();
    };
    // In order over one connection: HTTPS needs the clock.
    constexpr std::array kJobs{
        Job{"clock", &InternetSync::syncClock},
        Job{"cloud cover", &InternetSync::fetchCloudCover},
    };

    ESP_LOGI(kTag, "Syncing");
    bool ok = false;
    {
        const WifiSession wifi;
        if (wifi.connected()) {
            ok = true;
            for (const Job &job : kJobs) {
                const bool done = (this->*job.run)();
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

    const std::tm local  = localTime(std::chrono::system_clock::to_time_t(t));
    const auto    minute = std::chrono::minutes{local.tm_hour * 60 + local.tm_min};
    const auto    start  = std::chrono::minutes{window_start_.load()};
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
    self.run(); // the boot sync
    xSemaphoreGive(self.boot_done_);
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
