// Logging for the pure layers (sunsim::ev, sunsim::core), which also build
// on the host: std::format strings, checked at compile time, written through
// ESP-IDF's log on the target (its levels, timestamps, colors and
// esp_log_level_set) and to stdout on the host. A function, not a macro, so
// it travels through `import sunsim.ev`.
//
//   ev::log::info(kTag, "{} endpoint = {}", ev::roleName(role), endpoint);

module;

#if defined(ESP_PLATFORM)
#include "esp_log.h"
#endif

export module sunsim.ev:log;

import std.compat;

export namespace sunsim::ev::log {

enum class Level : uint8_t { Error, Warning, Info, Debug };

// Whether `tag` logs at `level` now: when not, nothing is formatted.
[[nodiscard]] bool enabled(Level level, const char *tag) noexcept;
void               write(Level level, const char *tag, std::string_view text);

template <class... Args> void at(Level level, const char *tag, std::format_string<Args...> format, Args &&...args)
{
    if (enabled(level, tag)) {
        write(level, tag, std::format(format, std::forward<Args>(args)...));
    }
}

template <class... Args> void error(const char *tag, std::format_string<Args...> format, Args &&...args)
{
    at(Level::Error, tag, format, std::forward<Args>(args)...);
}
template <class... Args> void warn(const char *tag, std::format_string<Args...> format, Args &&...args)
{
    at(Level::Warning, tag, format, std::forward<Args>(args)...);
}
template <class... Args> void info(const char *tag, std::format_string<Args...> format, Args &&...args)
{
    at(Level::Info, tag, format, std::forward<Args>(args)...);
}
template <class... Args> void debug(const char *tag, std::format_string<Args...> format, Args &&...args)
{
    at(Level::Debug, tag, format, std::forward<Args>(args)...);
}

} // namespace sunsim::ev::log

namespace sunsim::ev::log {

#if defined(ESP_PLATFORM)

namespace {
esp_log_level_t espLevel(Level level) noexcept
{
    switch (level) {
    case Level::Error:
        return ESP_LOG_ERROR;
    case Level::Warning:
        return ESP_LOG_WARN;
    case Level::Info:
        return ESP_LOG_INFO;
    case Level::Debug:
        break;
    }
    return ESP_LOG_DEBUG;
}
} // namespace

bool enabled(Level level, const char *tag) noexcept
{
    return esp_log_level_get(tag) >= espLevel(level);
}

void write(Level level, const char *tag, std::string_view text)
{
    const int size = static_cast<int>(text.size());
    switch (level) {
    case Level::Error:
        ESP_LOGE(tag, "%.*s", size, text.data());
        break;
    case Level::Warning:
        ESP_LOGW(tag, "%.*s", size, text.data());
        break;
    case Level::Info:
        ESP_LOGI(tag, "%.*s", size, text.data());
        break;
    case Level::Debug:
        ESP_LOGD(tag, "%.*s", size, text.data());
        break;
    }
}

#else

bool enabled(Level level, const char *) noexcept
{
    return level != Level::Debug; // the host tests' output stays readable
}

void write(Level level, const char *tag, std::string_view text)
{
    constexpr std::array kLetters{'E', 'W', 'I', 'D'};
    std::printf("%c (%s) %.*s\n", kLetters[std::to_underlying(level)], tag, static_cast<int>(text.size()), text.data());
}

#endif

} // namespace sunsim::ev::log
