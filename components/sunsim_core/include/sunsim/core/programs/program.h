#pragma once

#include <cstdint>
#include <optional>

#include "sunsim/ev/events.h"

namespace sunsim::core {

// A lighting program: decides what every bulb should show and posts it as
// LightTargets. It never deals with addresses, reachability or what was
// last sent; LightOutput does that, the same way for every program.
//
// ProgramHost owns the programs and runs one at a time (the Mode). Only the
// running program gets ticks and refreshes; every program gets settings, so
// it's current whenever it starts.
class Program {
public:
    explicit Program(ev::Post post) : post_(std::move(post)) {}
    virtual ~Program() = default;

    Program(const Program &)            = delete;
    Program &operator=(const Program &) = delete;

    // Became the running program: show it now. `now` is nullopt until the
    // clock's first Tick.
    virtual void start(const std::optional<ev::TimeReading> &now) { refresh(std::nullopt, now); }

    // Re-apply `role` (nullopt = every bulb) now, with a quick transition: a
    // bulb (re)joined or answers again, the network came up, a setting changed.
    virtual void refresh(std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) = 0;

    virtual void onTick(const ev::Tick &) {}               // 1 Hz, while running
    virtual void onProgramTick(const ev::ProgramTick &) {} // update interval, while running
    virtual void onConfig(const ev::ConfigChanged &) {}    // always, running or not

protected:
    // Calls f(role) for each bulb, or only for `only` if it's given (and a bulb).
    template <class F> static void forEachBulb(std::optional<ev::Role> only, F &&f)
    {
        for (const ev::Role role : ev::kBulbs) {
            if (!only || *only == role) {
                f(role);
            }
        }
    }

    void target(ev::Role role, const ev::LightState &state, ev::Transition level, ev::Transition color) const
    {
        post_(ev::LightTarget{role, state, level, color});
    }
    void target(ev::Role role, const ev::LightState &state, ev::Transition transition = {}) const
    {
        target(role, state, transition, transition);
    }

private:
    ev::Post post_;
};

} // namespace sunsim::core
