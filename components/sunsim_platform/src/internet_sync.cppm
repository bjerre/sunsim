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
import sunsim.weather;
import :wifi;

export namespace sunsim::platform {

// Everything that needs the internet, batched into short Wi-Fi sessions
// (Wi-Fi shares the 2.4 GHz radio with Zigbee). Each job has a cadence:
//
//   clock        SNTP                                      daily
//   forecast     Open-Meteo hourly cloud cover, today and  daily
//                tomorrow -> CloudCoverUpdated
//   conditions   Open-Meteo's weather now -> ConditionsUpdated   every 15 min
//   alerts       NWS's active alerts for the location -> AlertsUpdated   every 15 min
//
// Daily: once a day in the sync window (ProgramConfig, `config syncwindow`,
// default 04:00-05:00), retrying every 10 minutes within it on failure.
// Every job also runs at boot (start() waits for it, before the Zigbee radio
// starts) and when asked (SyncRequested: console `sync`, switching fast-day
// mode). A session runs whichever jobs are due, in the order above (HTTPS
// needs the clock); while the clock has never been set, every 10 minutes
// regardless: the lights stay off without it. Everything runs on its own
// task (TLS needs the stack); the replies are read as they stream in
// (sunsim.weather), whatever their size.
class InternetSync {
public:
    // The location is menuconfig's: the weather is for it.
    InternetSync(ev::Post post, double latitude_deg, double longitude_deg);

    InternetSync(const InternetSync &)            = delete;
    InternetSync &operator=(const InternetSync &) = delete;

    // Starts the task and waits for its first sync (the boot sync).
    void start();

    void onConfig(const ev::ConfigChanged &e);
    void onSyncRequested(const ev::SyncRequested &e);

private:
    struct Daily {};
    struct Every {
        std::chrono::minutes period;
    };
    using Cadence = std::variant<Daily, Every>;

    struct Job {
        const char *name;
        bool (InternetSync::*run)();
        Cadence when;
    };
    struct JobState {
        std::optional<std::chrono::sys_seconds> last_try;
        std::optional<std::chrono::sys_seconds> last_ok;
    };
    static constexpr std::size_t kJobCount = 4;
    static const std::array<Job, kJobCount> kJobs;

    void run(bool everything);
    bool syncClock();
    bool fetchCloudCover();
    bool fetchConditions();
    bool fetchAlerts();
    [[nodiscard]] bool due(const Job &job, const JobState &state, std::chrono::sys_seconds now) const;
    static void taskTrampoline(void *arg);

    ev::Post          post_;
    double            latitude_deg_;
    double            longitude_deg_;
    TaskHandle_t      task_      = nullptr;
    SemaphoreHandle_t boot_done_ = nullptr;

    // Set from the app task, read by the sync task: minutes after midnight.
    std::atomic<uint16_t> window_start_{4 * 60};
    std::atomic<uint16_t> window_end_{5 * 60};

    std::array<JobState, kJobCount> state_{}; // the sync task's own, by kJobs index
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

// A coordinate as the services like it: at most four decimals, no
// trailing zeros (NWS redirects "44.9800" to "44.98").
std::string coordinate(double degrees)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.4f", degrees);
    std::string_view s{text};
    while (s.ends_with('0')) {
        s.remove_suffix(1);
    }
    if (s.ends_with('.')) {
        s.remove_suffix(1);
    }
    return std::string{s};
}

struct Header {
    const char *name;
    const char *value;
};

// GET over HTTPS (checked against the certificate bundle), handing the body
// to `sink` a chunk at a time as it arrives. False on any failure (the sink
// may have seen part of the body).
bool httpsGet(const std::string &url, const std::function<void(std::string_view)> &sink,
              std::initializer_list<Header> headers = {})
{
    esp_http_client_config_t config{};
    config.url               = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms        = 30'000; // slow while Zigbee shares the radio
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    for (const Header &h : headers) {
        esp_http_client_set_header(client, h.name, h.value);
    }
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0) {
        const int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char chunk[512];
            int  n = 0;
            while ((n = esp_http_client_read(client, chunk, sizeof(chunk))) > 0) {
                sink({chunk, static_cast<std::size_t>(n)});
            }
            ok = n == 0;
        } else {
            ESP_LOGW(kTag, "HTTP %d from %s", status, url.c_str());
        }
    } else {
        ESP_LOGW(kTag, "Could not reach %s", url.c_str());
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
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
    const std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + coordinate(latitude_deg_) +
                            "&longitude=" + coordinate(longitude_deg_) +
                            "&hourly=cloud_cover&timeformat=unixtime&timezone=GMT&past_days=1&forecast_days=3";
    weather::ForecastReader reader;
    if (!httpsGet(url, [&](std::string_view chunk) { reader.feed(chunk); })) {
        return false;
    }

    const std::tm  today = localTime(std::time(nullptr));
    ev::CloudCover forecast;
    forecast.year      = static_cast<int16_t>(today.tm_year + 1900);
    forecast.yday      = static_cast<int16_t>(today.tm_yday);
    std::size_t placed = 0;
    for (const auto &[start, percent] : reader.hours()) {
        const std::tm local = localTime(std::chrono::system_clock::to_time_t(start));
        const long    slot  = (dateOf(local) - dateOf(today)).count() * 24 + local.tm_hour;
        if (slot >= 0 && slot < static_cast<long>(ev::CloudCover::kHours)) {
            forecast.percent[static_cast<std::size_t>(slot)] = percent;
            ++placed;
        }
    }
    if (placed == 0) {
        ESP_LOGW(kTag, "Unexpected forecast reply");
        return false;
    }
    ESP_LOGI(kTag, "Cloud cover, %u hours; today 08:00 %u%%, 12:00 %u%%, 16:00 %u%%", static_cast<unsigned>(placed),
             forecast.percent[8], forecast.percent[12], forecast.percent[16]);
    post_(ev::CloudCoverUpdated{forecast});
    return true;
}

// Open-Meteo's `current`: 15-minute model data (in the US, HRRR's).
bool InternetSync::fetchConditions()
{
    const std::string url = "https://api.open-meteo.com/v1/forecast?latitude=" + coordinate(latitude_deg_) +
                            "&longitude=" + coordinate(longitude_deg_) +
                            "&current=cloud_cover,weather_code&timeformat=unixtime";
    weather::ConditionsReader reader;
    if (!httpsGet(url, [&](std::string_view chunk) { reader.feed(chunk); })) {
        return false;
    }
    const auto now = reader.conditions();
    if (!now) {
        ESP_LOGW(kTag, "Unexpected conditions reply");
        return false;
    }
    ESP_LOGI(kTag, "Now: %u%% cloud, %s%s%s", now->cloud_percent,
             now->precip == ev::Precip::None ? "" : ev::intensityName(now->intensity),
             now->precip == ev::Precip::None ? "" : " ", ev::precipName(now->precip));
    post_(ev::ConditionsUpdated{.now = *now, .at = ev::BootClock::now()});
    return true;
}

// The National Weather Service's alerts in force for the location (US
// only; elsewhere the reply is empty). It asks for a User-Agent.
bool InternetSync::fetchAlerts()
{
    const std::string url = "https://api.weather.gov/alerts/active?status=actual&point=" +
                            coordinate(latitude_deg_) + "," + coordinate(longitude_deg_);
    weather::AlertsReader reader;
    if (!httpsGet(url, [&](std::string_view chunk) { reader.feed(chunk); },
                  {{"User-Agent", "sunsim (ESP32 daylight simulator)"}, {"Accept", "application/geo+json"}})) {
        return false;
    }
    const ev::Alerts alerts = reader.inForce(now(), ev::BootClock::now());
    if (alerts.count == 0) {
        ESP_LOGI(kTag, "No weather alerts");
    }
    for (const ev::Alert &alert : alerts.list()) {
        ESP_LOGI(kTag, "Alert: %s (%s)", alert.event.data(), ev::alertLevelName(alert.level));
    }
    post_(ev::AlertsUpdated{.alerts = alerts, .at = ev::BootClock::now()});
    return true;
}

// ---- Scheduling ----

// In order over one connection: HTTPS needs the clock.
const std::array<InternetSync::Job, InternetSync::kJobCount> InternetSync::kJobs{{
    {"clock", &InternetSync::syncClock, Daily{}},
    {"forecast", &InternetSync::fetchCloudCover, Daily{}},
    {"conditions", &InternetSync::fetchConditions, Every{15min}},
    {"alerts", &InternetSync::fetchAlerts, Every{15min}},
}};

// Runs the jobs that are due (every one if `everything`), in one Wi-Fi
// session; none due, no session.
void InternetSync::run(bool everything)
{
    std::array<bool, kJobCount> chosen{};
    for (std::size_t i = 0; i < kJobCount; ++i) {
        chosen[i] = everything || due(kJobs[i], state_[i], now());
    }
    if (std::ranges::none_of(chosen, std::identity{})) {
        return;
    }

    ESP_LOGI(kTag, "Syncing");
    const WifiSession wifi;
    for (std::size_t i = 0; i < kJobCount; ++i) {
        if (!chosen[i]) {
            continue;
        }
        // Without a clock HTTPS can't check certificates: don't try.
        const bool clock = kJobs[i].run == &InternetSync::syncClock;
        const bool done  = wifi.connected() && (clock || now() >= kClockSetAfter) && (this->*kJobs[i].run)();
        ESP_LOGI(kTag, "  %s: %s", kJobs[i].name, done ? "ok" : "failed");
        // Read after the job: the clock sync itself moves the clock.
        state_[i].last_try = now();
        if (done) {
            state_[i].last_ok = state_[i].last_try;
        }
    }
} // Wi-Fi off

bool InternetSync::due(const Job &job, const JobState &state, std::chrono::sys_seconds t) const
{
    const bool retry_ready = !state.last_try || t - *state.last_try >= kRetryDelay;
    if (t < kClockSetAfter) {
        return retry_ready; // never set: keep trying, window or not
    }
    if (const auto *every = std::get_if<Every>(&job.when)) {
        return !state.last_try || t - *state.last_try >= every->period;
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
    if (state.last_ok && *state.last_ok >= opened) {
        return false;
    }
    return !state.last_try || *state.last_try < opened || retry_ready;
}

void InternetSync::taskTrampoline(void *arg)
{
    auto &self = *static_cast<InternetSync *>(arg);
    self.run(true); // the boot sync
    xSemaphoreGive(self.boot_done_);
    for (;;) {
        // A minute at a time (a day in ticks would overflow pdMS_TO_TICKS),
        // or at once when asked.
        const bool asked = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60'000)) > 0;
        self.run(asked);
    }
}

} // namespace sunsim::platform
