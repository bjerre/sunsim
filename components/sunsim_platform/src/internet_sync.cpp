module;

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_pthread.h"
#include "freertos/FreeRTOS.h" // pdMS_TO_TICKS, for the SNTP wait

module sunsim.platform;

import std.compat;
import sunsim.ev;
import sunsim.weather;
import :wifi;
import :zone;

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

struct InternetSync::Task {
    std::binary_semaphore     boot_done{0};
    std::counting_semaphore<> asked{0}; // SyncRequested: counting, so asking twice is harmless
    std::jthread              thread;   // last: it waits on the two above
};

InternetSync::InternetSync(ev::Post post, double latitude_deg, double longitude_deg)
    : post_(std::move(post)), latitude_deg_(latitude_deg), longitude_deg_(longitude_deg),
      task_(std::make_unique<Task>())
{
}

InternetSync::~InternetSync()
{
    task_->thread.request_stop();
    task_->asked.release(); // wake it to see the stop; task_ then joins it
}

void InternetSync::start()
{
    // ESP-IDF's std::threads take their stack, name and priority from here:
    // 8 KB, as TLS needs.
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_cfg_t       config   = defaults;
    config.stack_size                = 8192;
    config.prio                      = 3;
    config.thread_name               = "internet";
    esp_pthread_set_cfg(&config);
    task_->thread = std::jthread{[this](std::stop_token stop) { loop(std::move(stop)); }};
    esp_pthread_set_cfg(&defaults);

    task_->boot_done.acquire();
}

void InternetSync::on(const ev::ConfigChanged &e)
{
    window_start_ = static_cast<uint16_t>(e.config.sync.start.count());
    window_end_   = static_cast<uint16_t>(e.config.sync.end.count());
}

void InternetSync::on(const ev::SyncRequested &)
{
    task_->asked.release();
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

    const auto     today = std::chrono::floor<std::chrono::days>(toLocal(now()));
    ev::CloudCover forecast;
    forecast.day       = today;
    std::size_t placed = 0;
    for (const auto &[start, percent] : reader.hours()) {
        const auto slot = std::chrono::floor<std::chrono::hours>(toLocal(start) - today).count();
        if (slot >= 0 && std::cmp_less(slot, ev::CloudCover::kHours)) {
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

    const auto        local = toLocal(t);
    const auto        since = local - std::chrono::floor<std::chrono::days>(local); // since midnight
    const ev::DaySpan window{std::chrono::minutes{window_start_.load()}, std::chrono::minutes{window_end_.load()}};
    if (!window.contains(since)) {
        return false;
    }
    // When today's window opened: done since then, nothing to do; else the
    // first try in it, or a retry.
    const auto opened = t - (since - window.start);
    if (state.last_ok && *state.last_ok >= opened) {
        return false;
    }
    return !state.last_try || *state.last_try < opened || retry_ready;
}

void InternetSync::loop(std::stop_token stop)
{
    using namespace std::chrono_literals;
    run(true); // the boot sync
    task_->boot_done.release();
    while (!stop.stop_requested()) {
        // A minute at a time, or at once when asked (once for however many asks).
        const bool asked = task_->asked.try_acquire_for(1min);
        while (task_->asked.try_acquire()) {
        }
        run(asked);
    }
}

} // namespace sunsim::platform
