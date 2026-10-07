#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

#include "ezbee/core_types.h"

namespace sunsim::platform {

// A Zigbee light endpoint identified by its short address, addressed
// directly (no binding table involved) from sunsim's light-control
// endpoint. Cheap, copyable value type - a BulbEndpoint is just an address.
// Each command returns the stack's result for queuing it (not delivery);
// EZB_ERR_NONE when it was accepted for sending.
class BulbEndpoint {
public:
    // ZCL transition times are expressed in tenths of a second.
    using TransitionTime = std::chrono::duration<uint16_t, std::ratio<1, 10>>;

    constexpr BulbEndpoint(uint16_t short_addr, uint8_t endpoint, uint8_t source_endpoint) noexcept
        : short_addr_(short_addr), endpoint_(endpoint), source_endpoint_(source_endpoint)
    {
    }

    ezb_err_t turnOn() const;
    ezb_err_t turnOff() const;
    ezb_err_t setLevel(uint8_t level_0_254, TransitionTime transition = TransitionTime{1}) const;
    ezb_err_t setColorTemperature(uint16_t mireds, TransitionTime transition = TransitionTime{1}) const;
    ezb_err_t setHueSaturation(uint8_t hue, uint8_t saturation, TransitionTime transition = TransitionTime{1}) const;
    // CIE 1931 xy chromaticity (each 0..1), for colours outside the bulb's
    // colour-temperature range (e.g. amber below ~2700K).
    ezb_err_t setColorXY(double x, double y, TransitionTime transition = TransitionTime{1}) const;

    [[nodiscard]] constexpr uint16_t shortAddress() const noexcept { return short_addr_; }
    [[nodiscard]] constexpr uint8_t endpoint() const noexcept { return endpoint_; }

private:
    uint16_t short_addr_;
    uint8_t  endpoint_;
    uint8_t  source_endpoint_;
};

} // namespace sunsim::platform
