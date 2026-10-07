#pragma once

// Logging for code that must also build on the host (sunsim::ev, sunsim::core):
// ESP_LOGx on the target, printf on the host (debug level compiled out there).
#if defined(ESP_PLATFORM)
#include "esp_log.h"
#define SUNSIM_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define SUNSIM_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define SUNSIM_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define SUNSIM_LOGD(tag, ...) ESP_LOGD(tag, __VA_ARGS__)
#else
#include <cstdio>
#define SUNSIM_HOST_LOG(level, tag, fmt, ...) std::printf(level " (%s) " fmt "\n", tag __VA_OPT__(, ) __VA_ARGS__)
#define SUNSIM_LOGE(tag, fmt, ...) SUNSIM_HOST_LOG("E", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SUNSIM_LOGW(tag, fmt, ...) SUNSIM_HOST_LOG("W", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SUNSIM_LOGI(tag, fmt, ...) SUNSIM_HOST_LOG("I", tag, fmt __VA_OPT__(, ) __VA_ARGS__)
#define SUNSIM_LOGD(tag, fmt, ...) ((void)0)
#endif
