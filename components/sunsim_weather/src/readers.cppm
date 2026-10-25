export module sunsim.weather:readers;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import :json;

export namespace sunsim::weather {

// ---- What the codes mean ---------------------------------------------------

// What's falling for a WMO weather code (Open-Meteo's weather_code); None
// for clear, cloud and fog.
[[nodiscard]] constexpr std::pair<ev::Precip, ev::Intensity> precipOf(int wmo_code) noexcept
{
    using enum ev::Precip;
    using enum ev::Intensity;
    switch (wmo_code) {
    case 51: return {Drizzle, Light};
    case 53: return {Drizzle, Moderate};
    case 55: return {Drizzle, Heavy};
    case 56: return {FreezingRain, Light};  // freezing drizzle
    case 57: return {FreezingRain, Moderate};
    case 61: case 80: return {Rain, Light}; // 80-82: showers
    case 63: case 81: return {Rain, Moderate};
    case 65: case 82: return {Rain, Heavy};
    case 66: return {FreezingRain, Light};
    case 67: return {FreezingRain, Heavy};
    case 71: case 77: case 85: return {Snow, Light}; // 77: snow grains; 85-86: showers
    case 73: return {Snow, Moderate};
    case 75: case 86: return {Snow, Heavy};
    case 95: return {Thunderstorm, Moderate};
    case 96: case 99: return {Thunderstorm, Heavy}; // with hail
    default: return {None, Light};
    }
}

// An NWS alert's tier, by its event name's last word ("Tornado Watch");
// failing that, Extreme severity is a warning and Severe a watch. Other
// statements and outlooks: None.
[[nodiscard]] ev::AlertLevel alertLevelOf(std::string_view event, std::string_view severity) noexcept;

// An ISO 8601 time with its offset, as NWS gives them:
// "2026-10-16T18:00:00-05:00" (or "...Z").
[[nodiscard]] std::optional<std::chrono::sys_seconds> parseIsoTime(std::string_view text) noexcept;

// ---- Open-Meteo -----------------------------------------------------------

// The hourly forecast: `hourly=cloud_cover&timeformat=unixtime`.
class ForecastReader {
public:
    struct Hour {
        std::chrono::sys_seconds start;
        uint8_t                  cloud_percent;
    };

    ForecastReader();
    ForecastReader(const ForecastReader &)            = delete;
    ForecastReader &operator=(const ForecastReader &) = delete;

    void feed(std::string_view chunk) { json_.feed(chunk); }
    // The hours with data, or empty if the reply wasn't a forecast.
    [[nodiscard]] std::vector<Hour> hours() const;

private:
    std::vector<std::optional<double>> times_;
    std::vector<std::optional<double>> covers_;
    JsonScanner                        json_;
};

// The weather now: `current=cloud_cover,weather_code`.
class ConditionsReader {
public:
    ConditionsReader();
    ConditionsReader(const ConditionsReader &)            = delete;
    ConditionsReader &operator=(const ConditionsReader &) = delete;

    void feed(std::string_view chunk) { json_.feed(chunk); }
    [[nodiscard]] std::optional<ev::Conditions> conditions() const;

private:
    std::optional<double> cloud_;
    std::optional<double> code_;
    JsonScanner           json_;
};

// ---- NWS ------------------------------------------------------------------

// The active alerts: api.weather.gov `/alerts/active?point=...`, GeoJSON.
// Keeps only actual alerts at a tier (alertLevelOf), not cancellations.
class AlertsReader {
public:
    struct Alert {
        ev::AlertLevel                          level = ev::AlertLevel::None;
        std::string                             event;
        std::optional<std::chrono::sys_seconds> ends; // `ends`, else `expires`
    };

    AlertsReader();
    AlertsReader(const AlertsReader &)            = delete;
    AlertsReader &operator=(const AlertsReader &) = delete;

    void feed(std::string_view chunk) { json_.feed(chunk); }
    [[nodiscard]] const std::vector<Alert> &alerts() const noexcept { return alerts_; }

    // What the lights get: those not ended by `wall_now`, one per event
    // name (the latest ending), highest tier first, at most Alerts::kMax.
    // Each one's end becomes a BootClock time from `boot_now` (the same
    // moment); one without an end lasts an hour.
    [[nodiscard]] ev::Alerts inForce(std::chrono::sys_seconds wall_now, ev::Instant boot_now) const;

private:
    struct Fields {
        std::string event, severity, status, message_type, ends, expires;
    };
    void finish();

    Fields             fields_;
    std::vector<Alert> alerts_;
    JsonScanner        json_;
};

} // namespace sunsim::weather

namespace sunsim::weather {

ev::AlertLevel alertLevelOf(std::string_view event, std::string_view severity) noexcept
{
    if (event.ends_with("Warning") || event.ends_with("Emergency")) {
        return ev::AlertLevel::Warning;
    }
    if (event.ends_with("Watch")) {
        return ev::AlertLevel::Watch;
    }
    if (event.ends_with("Advisory")) {
        return ev::AlertLevel::Advisory;
    }
    if (severity == "Extreme") {
        return ev::AlertLevel::Warning;
    }
    if (severity == "Severe") {
        return ev::AlertLevel::Watch;
    }
    return ev::AlertLevel::None;
}

std::optional<std::chrono::sys_seconds> parseIsoTime(std::string_view text) noexcept
{
    const std::string s{text}; // sscanf wants it NUL-terminated
    int  y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, used = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &used) != 6) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{y}, std::chrono::month{static_cast<unsigned>(mo)},
                                           std::chrono::day{static_cast<unsigned>(d)}};
    if (!date.ok()) {
        return std::nullopt;
    }
    std::chrono::sys_seconds t = std::chrono::sys_days{date} + std::chrono::hours{h} + std::chrono::minutes{mi} +
                                 std::chrono::seconds{sec};
    const std::string_view zone = std::string_view{s}.substr(static_cast<std::size_t>(used));
    if (zone == "Z") {
        return t;
    }
    int oh = 0, om = 0;
    if (zone.size() != 6 || (zone[0] != '+' && zone[0] != '-') ||
        std::sscanf(s.c_str() + used + 1, "%2d:%2d", &oh, &om) != 2) {
        return std::nullopt;
    }
    const auto offset = std::chrono::hours{oh} + std::chrono::minutes{om};
    return zone[0] == '-' ? t + offset : t - offset; // local = UTC + offset
}

// ---- ForecastReader -------------------------------------------------------

ForecastReader::ForecastReader()
    : json_([this](const JsonScanner::Scalar &s) {
          if (s.parent != "hourly") {
              return;
          }
          if (s.key == "time") {
              times_.push_back(s.number());
          } else if (s.key == "cloud_cover") {
              covers_.push_back(s.number()); // null: nullopt
          }
      })
{
}

std::vector<ForecastReader::Hour> ForecastReader::hours() const
{
    std::vector<Hour> out;
    if (times_.size() != covers_.size()) {
        return out;
    }
    for (std::size_t i = 0; i < times_.size(); ++i) {
        if (times_[i] && covers_[i]) {
            out.push_back({.start         = std::chrono::sys_seconds{std::chrono::seconds{std::llround(*times_[i])}},
                           .cloud_percent = static_cast<uint8_t>(std::clamp(std::lround(*covers_[i]), 0L, 100L))});
        }
    }
    return out;
}

// ---- ConditionsReader -----------------------------------------------------

ConditionsReader::ConditionsReader()
    : json_([this](const JsonScanner::Scalar &s) {
          if (s.parent != "current") {
              return; // also "current_units", with the same keys
          }
          if (s.key == "cloud_cover") {
              cloud_ = s.number();
          } else if (s.key == "weather_code") {
              code_ = s.number();
          }
      })
{
}

std::optional<ev::Conditions> ConditionsReader::conditions() const
{
    if (!cloud_ || !code_) {
        return std::nullopt;
    }
    const auto [precip, intensity] = precipOf(static_cast<int>(std::lround(*code_)));
    return ev::Conditions{.cloud_percent = static_cast<uint8_t>(std::clamp(std::lround(*cloud_), 0L, 100L)),
                          .precip        = precip,
                          .intensity     = intensity};
}

// ---- AlertsReader ---------------------------------------------------------

AlertsReader::AlertsReader()
    : json_(
          [this](const JsonScanner::Scalar &s) {
              if (s.parent != "properties" || !s.is_string) {
                  return; // null `ends` stays empty
              }
              const std::pair<std::string_view, std::string *> wanted[] = {
                  {"event", &fields_.event},           {"severity", &fields_.severity},
                  {"status", &fields_.status},         {"messageType", &fields_.message_type},
                  {"ends", &fields_.ends},             {"expires", &fields_.expires},
              };
              for (const auto &[key, field] : wanted) {
                  if (s.key == key) {
                      field->assign(s.text);
                  }
              }
          },
          [this](std::string_view key) {
              if (key == "properties") {
                  finish();
              }
          })
{
}

void AlertsReader::finish()
{
    const Fields f = std::exchange(fields_, {});
    if (f.status != "Actual" || f.message_type == "Cancel") {
        return;
    }
    const ev::AlertLevel level = alertLevelOf(f.event, f.severity);
    if (level == ev::AlertLevel::None) {
        return;
    }
    alerts_.push_back({.level = level, .event = f.event, .ends = parseIsoTime(f.ends).or_else([&] {
                                                                return parseIsoTime(f.expires);
                                                            })});
}

ev::Alerts AlertsReader::inForce(std::chrono::sys_seconds wall_now, ev::Instant boot_now) const
{
    using namespace std::chrono_literals;

    // One per event name, the latest ending: an update repeats its alert.
    std::vector<Alert> latest;
    for (const Alert &a : alerts_) {
        const auto ends = a.ends.value_or(wall_now + 1h);
        if (ends <= wall_now) {
            continue;
        }
        const auto same = std::ranges::find(latest, a.event, &Alert::event);
        if (same == latest.end()) {
            latest.push_back({a.level, a.event, ends});
        } else if (ends > *same->ends) {
            same->ends = ends;
        }
    }
    std::ranges::stable_sort(latest, std::ranges::greater{}, &Alert::level);

    ev::Alerts out;
    for (const Alert &a : latest | std::views::take(ev::Alerts::kMax)) {
        ev::Alert &alert = out.items[out.count++];
        alert.level      = a.level;
        alert.until      = boot_now + (*a.ends - wall_now);
        const auto name  = std::string_view{a.event}.substr(0, alert.event.size() - 1);
        std::ranges::copy(name, alert.event.begin());
    }
    return out;
}

} // namespace sunsim::weather
