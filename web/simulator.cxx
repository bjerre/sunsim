#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define SUNSIM_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define SUNSIM_KEEPALIVE
#endif

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.core;
import sunsim.ev;
import sunsim.presentation;

namespace {

using namespace std::chrono_literals;

std::string jsonString(std::string_view text)
{
    std::string out{"\""};
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0x0f];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

const char *toneName(sunsim::presentation::Tone tone)
{
    using sunsim::presentation::Tone;
    switch (tone) {
    case Tone::Muted:
        return "muted";
    case Tone::Daylight:
        return "daylight";
    case Tone::Tuning:
        return "tuning";
    case Tone::Advisory:
        return "advisory";
    case Tone::Watch:
        return "watch";
    case Tone::Warning:
        return "warning";
    case Tone::Sky:
        return "sky";
    }
    return "muted";
}

const char *linkName(sunsim::ev::DeviceLink link)
{
    using sunsim::ev::DeviceLink;
    switch (link) {
    case DeviceLink::Unpaired:
        return "unpaired";
    case DeviceLink::Silent:
        return "silent";
    case DeviceLink::Online:
        return "online";
    }
    return "unpaired";
}

const char *pressName(sunsim::ev::Press press)
{
    using sunsim::ev::Press;
    switch (press) {
    case Press::Single:
        return "single";
    case Press::Double:
        return "double";
    case Press::Hold:
        return "hold";
    case Press::Ambiguous:
        return nullptr;
    }
    return nullptr;
}

struct VirtualOffice {
    struct OutputMetadata {
        sunsim::ev::WeatherEffect weather_effect = sunsim::ev::WeatherEffect::None;
        sunsim::ev::PulsePhase    pulse_phase    = sunsim::ev::PulsePhase::None;
    };

    sunsim::core::CoreRuntime runtime{sunsim::ev::Addresses{}};
    sunsim::ev::Instant now{};
    sunsim::ev::TimeReading time{};
    sunsim::ev::ProgramConfig config{};
    sunsim::ev::StatusSnapshot status{};
    sunsim::ev::PerRole<sunsim::ev::LightState> lights{};
    sunsim::ev::PerRole<OutputMetadata> output_metadata{};
    std::vector<sunsim::ev::Instant> wakes;
    std::string state_json;
    bool have_status = false;

    VirtualOffice()
    {
        auto &bus = runtime.bus();
        bus.on<sunsim::ev::StatusChanged>([this](const auto &e) {
            if (e.snapshot) {
                status = *e.snapshot;
                have_status = true;
            }
        });
        bus.on<sunsim::ev::OutputTarget>([this](const auto &e) {
            if (sunsim::ev::isLight(e.role)) {
                output_metadata[e.role] = {e.weather_effect, e.pulse_phase};
            }
        });
        bus.on<sunsim::ev::SetLight>([this](const auto &e) {
            for (const auto role : sunsim::ev::lights()) {
                const auto &record = status.records[role];
                if (record.valid && record.short_addr == e.short_addr) {
                    auto &light = lights[role];
                    if (e.power) {
                        light.on = *e.power;
                    }
                    if (e.level) {
                        light.level = *e.level;
                    }
                    if (e.color) {
                        light.color = *e.color;
                    }
                    break;
                }
            }
        });
        bus.on<sunsim::ev::Discover>([this](const auto &e) {
            runtime.post()(sunsim::ev::EndpointFound{.role = e.role, .endpoint = 1});
        });
        bus.on<sunsim::ev::Bind>([this](const auto &e) {
            runtime.post()(sunsim::ev::BindResult{.role = e.role, .ok = true});
        });
        bus.on<sunsim::ev::Ping>([this](const auto &e) {
            runtime.post()(sunsim::ev::PingResult{.role = e.role, .ok = true});
        });
        bus.on<sunsim::ev::WakeAt>([this](const auto &e) { wakes.push_back(e.at); });

        reset();
    }

    void reset()
    {
        now = {};
        time = {.local = std::chrono::local_seconds{
                    std::chrono::local_days{std::chrono::year{2022} / std::chrono::June / 21} + 12h},
                .utc_offset = 0min,
                .valid = true,
                .simulated = true};
        status = {};
        have_status = false;
        lights = {};
        output_metadata = {};
        wakes.clear();

        config = {};
        runtime.post()(sunsim::ev::ConfigChanged{.config = config});

        sunsim::ev::DeviceRecords records;
        for (const auto role : sunsim::ev::kRoles) {
            const auto id = static_cast<uint16_t>(sunsim::ev::slot(role) + 1);
            records[role] = {.valid = true,
                             .ext_addr = 0x00124b0000000000ULL + id,
                             .short_addr = static_cast<uint16_t>(0x1000 + id),
                             .endpoint = 1};
        }
        runtime.post()(sunsim::ev::RolesLoaded{records});
        runtime.post()(sunsim::ev::NetworkUp{.at = now});
        for (const auto role : sunsim::ev::kRoles) {
            const auto &record = records[role];
            runtime.post()(sunsim::ev::DeviceAnnounced{.ext_addr = record.ext_addr,
                                                       .short_addr = record.short_addr});
        }
        runtime.drain();
        tick();
    }

    void tick()
    {
        runtime.post()(sunsim::ev::Tick{.at = now, .time = time});
        runtime.post()(sunsim::ev::ProgramTick{.time = time, .transition = 60s});
        runtime.drain();
    }

    void advance(int seconds)
    {
        // Advance adapter time for scheduled device events. The browser sends
        // its local wall clock separately through setClock().
        const int count = std::clamp(seconds, 0, 60);
        for (int i = 0; i < count; ++i) {
            now += 1s;
            std::ranges::sort(wakes);
            while (!wakes.empty() && wakes.front() <= now) {
                const auto at = wakes.front();
                wakes.erase(wakes.begin());
                runtime.post()(sunsim::ev::Wake{.at = at});
            }
            runtime.drain();
        }
    }

    void setMinute(int minute)
    {
        using namespace std::chrono;
        const auto clamped = std::clamp(minute, 0, 1439);
        time.local = local_seconds{time.day() + minutes{clamped}};
        tick();
    }

    bool setLocation(double latitude, double longitude)
    {
        if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90.0 || latitude > 90.0 ||
            longitude < -180.0 || longitude > 180.0) {
            return false;
        }
        config.latitude_deg = latitude;
        config.longitude_deg = longitude;
        runtime.post()(sunsim::ev::ConfigChanged{.config = config});
        runtime.drain();
        tick();
        return true;
    }

    bool setClock(int yearValue, int monthValue, int dayValue, int hourValue, int minuteValue, int utcOffsetMinutes)
    {
        using namespace std::chrono;
        const year_month_day date{year{yearValue}, month{static_cast<unsigned>(monthValue)},
                                  day{static_cast<unsigned>(dayValue)}};
        if (!date.ok() || hourValue < 0 || hourValue > 23 || minuteValue < 0 || minuteValue > 59 ||
            utcOffsetMinutes < -14 * 60 || utcOffsetMinutes > 14 * 60) {
            return false;
        }
        time.local = local_seconds{local_days{date} + hours{hourValue} + minutes{minuteValue}};
        time.utc_offset = minutes{utcOffsetMinutes};
        tick();
        return true;
    }

    void press(int kind)
    {
        using sunsim::ev::Press;
        const auto press = kind == 1 ? Press::Double : kind == 2 ? Press::Hold : Press::Single;
        runtime.post()(sunsim::ev::ButtonPressed{.button = {sunsim::ev::Group::Button, 0},
                                                 .press = press,
                                                 .source = sunsim::ev::Source::Zigbee,
                                                 .at = now});
        runtime.drain();
        tick();
    }

    void weather(int precip, int cloud)
    {
        using sunsim::ev::Intensity;
        using sunsim::ev::Precip;
        const auto kind = precip >= 0 && precip <= 5 ? static_cast<Precip>(precip) : Precip::None;
        // The core deliberately uses forecast cloud during simulated time
        // and ignores live cloud readings. Model the browser slider as a
        // simple all-day forecast while keeping precipitation a live input.
        sunsim::ev::CloudCover forecast;
        forecast.day = time.day();
        forecast.percent.fill(static_cast<uint8_t>(std::clamp(cloud, 0, 100)));
        runtime.post()(sunsim::ev::CloudCoverUpdated{.forecast = forecast});
        runtime.post()(sunsim::ev::ConditionsUpdated{
            .now = {.cloud_percent = static_cast<uint8_t>(std::clamp(cloud, 0, 100)),
                    .precip = kind,
                    .intensity = Intensity::Moderate},
            .at = now});
        runtime.drain();
        tick();
    }

    void alert(int level)
    {
        sunsim::ev::Alerts alerts;
        const int clamped = std::clamp(level, 0, 3);
        if (clamped != 0) {
            alerts.count = 1;
            alerts.items[0].level = static_cast<sunsim::ev::AlertLevel>(clamped);
            constexpr std::string_view name = "Browser simulation alert";
            std::ranges::copy(name, alerts.items[0].event.begin());
            alerts.items[0].until = now + 1h;
        }
        runtime.post()(sunsim::ev::AlertsUpdated{.alerts = alerts, .at = now});
        runtime.drain();
        tick();
    }

    void appendColor(std::string &out, const sunsim::ev::Color &color)
    {
        std::visit(
            [&](const auto &value) {
                using C = std::decay_t<decltype(value)>;
                if constexpr (std::same_as<C, sunsim::ev::Mireds>) {
                    out += std::format("{{\"mode\":\"mireds\",\"value\":{}}}", value.value);
                } else if constexpr (std::same_as<C, sunsim::ev::Xy>) {
                    out += std::format("{{\"mode\":\"xy\",\"x\":{},\"y\":{}}}", value.x, value.y);
                } else {
                    out += std::format("{{\"mode\":\"hue\",\"hue\":{},\"saturation\":{}}}", value.hue,
                                       value.saturation);
                }
            },
            color);
    }

    void appendDaylightCurve(std::string &out, sunsim::ev::Role role)
    {
        out += ",\"curve\":[";
        if (!status.time.valid) {
            out += ']';
            return;
        }

        constexpr std::size_t sample_count = 97;
        const double cloud = status.sky.cloud_percent.value_or(0) / 100.0;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const int minute = static_cast<int>(std::min(i * 15, 1439uz));
            auto sample_time = status.time;
            sample_time.local = std::chrono::local_seconds{
                status.time.day() + std::chrono::minutes{minute}};
            const auto target = sunsim::core::daylightTarget(
                role, status.config, status.peaks, status.min_levels, sample_time, cloud);
            if (i != 0) {
                out += ',';
            }
            out += std::format("{{\"minute\":{},\"on\":{},\"level\":{},\"color\":",
                               minute, target.on ? "true" : "false", target.level);
            appendColor(out, target.color);
            out += '}';
        }
        out += ']';
    }

    std::string json()
    {
        using sunsim::presentation::makeStatusDisplayData;
        using sunsim::presentation::makeProgramFlowDisplayData;
        const auto display = makeStatusDisplayData(status);
        const auto flow = makeProgramFlowDisplayData(status);
        const std::chrono::year_month_day sim_date{time.day()};
        const auto sun = sunsim::core::computeSunPosition(
            config.latitude_deg, config.longitude_deg, time.dayOfYear(),
            sunsim::ev::Hours{time.sinceMidnight()}, sunsim::ev::Hours{time.utc_offset});
        std::string out = std::format(
            "{{\"location\":{{\"latitude\":{},\"longitude\":{}}},\"utcOffsetMinutes\":{},"
            "\"time\":{},\"weekday\":{},\"simDate\":{},"
            "\"sun\":{{\"azimuthDeg\":{},\"elevationDeg\":{}}},"
            "\"dayLength\":{},\"nextEvent\":{},\"mode\":{},"
            "\"modeTone\":{},\"sky\":{},\"skyTone\":{},\"simMinute\":{},\"programFlow\":[",
            config.latitude_deg, config.longitude_deg, time.utc_offset.count(), jsonString(display.time),
            jsonString(display.weekday),
            jsonString(std::format("{:04}-{:02}-{:02}", static_cast<int>(sim_date.year()),
                                   static_cast<unsigned>(sim_date.month()), static_cast<unsigned>(sim_date.day()))),
            sun.azimuth_deg, sun.elevation_deg,
            jsonString(display.day_length), jsonString(display.next_event),
            jsonString(display.mode), jsonString(toneName(display.mode_tone)), jsonString(display.sky),
            jsonString(toneName(display.sky_tone)),
            std::chrono::duration_cast<std::chrono::minutes>(time.sinceMidnight()).count());
        for (std::size_t i = 0; i < flow.zone_count; ++i) {
            const auto &zone = flow.zones[i];
            if (i != 0) {
                out += ',';
            }
            out += std::format("{{\"zone\":{},\"mode\":{},\"gateOpen\":{},\"gateApplies\":{},\"availableModes\":[",
                               jsonString(zone.name), jsonString(sunsim::ev::modeName(zone.mode)),
                               zone.gate_open ? "true" : "false", zone.gate_applies ? "true" : "false");
            for (std::size_t j = 0; j < zone.mode_count; ++j) {
                if (j != 0) {
                    out += ',';
                }
                const auto &choice = zone.available_modes[j];
                const char *press = choice.button_press ? pressName(*choice.button_press) : nullptr;
                out += std::format("{{\"mode\":{},\"buttonPress\":{}}}",
                                   jsonString(sunsim::ev::modeName(choice.mode)),
                                   press ? jsonString(press) : "null");
            }
            out += "],\"members\":[";
            for (std::size_t j = 0; j < zone.member_count; ++j) {
                const auto &member = zone.members[j];
                if (j != 0) {
                    out += ',';
                }
                out += std::format("{{\"role\":{},\"behavior\":{}",
                                   jsonString(sunsim::ev::roleName(member.role)),
                                   jsonString(member.behavior));
                appendDaylightCurve(out, member.role);
                out += '}';
            }
            out += "]}";
        }
        out += "],\"lights\":[";
        bool first = true;
        for (const auto role : sunsim::ev::lights()) {
            const auto &light = lights[role];
            if (!first) {
                out += ',';
            }
            first = false;
            const auto &metadata = output_metadata[role];
            out += std::format(
                "{{\"role\":{},\"on\":{},\"level\":{},\"weatherEffect\":{},\"pulsePhase\":{},\"color\":",
                jsonString(sunsim::ev::roleName(role)), light.on ? "true" : "false", light.level,
                jsonString(sunsim::ev::weatherEffectName(metadata.weather_effect)),
                jsonString(sunsim::ev::pulsePhaseName(metadata.pulse_phase)));
            appendColor(out, light.color);
            out += '}';
        }
        out += "],\"devices\":[";
        first = true;
        for (const auto role : sunsim::ev::kRoles) {
            if (!first) {
                out += ',';
            }
            first = false;
            const auto &device = status.devices[role];
            out += std::format("{{\"role\":{},\"link\":{},\"endpoint\":{}}}",
                               jsonString(sunsim::ev::roleName(role)), jsonString(linkName(device.link)),
                               device.has_endpoint ? "true" : "false");
        }
        out += "]}";
        return out;
    }
};

VirtualOffice &office()
{
    static VirtualOffice instance;
    return instance;
}

} // namespace

extern "C" {

SUNSIM_KEEPALIVE void sunsim_init() { (void)office(); }

SUNSIM_KEEPALIVE void sunsim_advance(int seconds) { office().advance(seconds); }

SUNSIM_KEEPALIVE void sunsim_set_minute(int minute) { office().setMinute(minute); }

SUNSIM_KEEPALIVE int sunsim_set_location(double latitude, double longitude)
{
    return office().setLocation(latitude, longitude) ? 1 : 0;
}

SUNSIM_KEEPALIVE int sunsim_set_clock(int year, int month, int day, int hour, int minute, int utcOffsetMinutes)
{
    return office().setClock(year, month, day, hour, minute, utcOffsetMinutes) ? 1 : 0;
}

SUNSIM_KEEPALIVE void sunsim_press(int kind) { office().press(kind); }

SUNSIM_KEEPALIVE void sunsim_weather(int precip, int cloud) { office().weather(precip, cloud); }

SUNSIM_KEEPALIVE void sunsim_alert(int level) { office().alert(level); }

SUNSIM_KEEPALIVE const char *sunsim_state()
{
    office().state_json = office().json();
    return office().state_json.c_str();
}

} // extern "C"
