#pragma once

#include <cstdint>
#include <memory>

#include "esp_timer.h"

#include "sunsim/ev/events.h"

namespace sunsim::platform {

class FastDayClock;

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
