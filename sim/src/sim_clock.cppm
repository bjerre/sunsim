export module sunsim.sim:sim_clock;

import std.compat;
import sunsim.ev;
import :scheduler;

export namespace sunsim::sim {

// The board's platform::Clock on virtual time: a Heartbeat every second,
// each a Tick with the wall-clock time (`start` plus the time run), a
// ProgramTick every 60 s, and a Wake at each time a WakeAt asks for. (No
// fast-day mode: virtual time runs a day as fast as you like.) (Defined in
// sim_clock.cpp, a module implementation unit: defined here, GCC 15 crashes
// writing the module interface, as with platform::Clock; see
// ARCHITECTURE.md §12.)
class SimClock {
public:
    static constexpr std::chrono::seconds kUpdateInterval{60};

    SimClock(ev::Post post, Scheduler &scheduler, ev::TimeReading start);

    void start(); // the first Heartbeat a second from now

    void on(const ev::Heartbeat &e);
    void on(const ev::WakeAt &e);
    void on(const ev::Wake &e);

    [[nodiscard]] ev::TimeReading read() const;

private:
    ev::Post                   post_;
    Scheduler                 &scheduler_;
    ev::TimeReading            start_;
    ev::Instant                started_{};
    std::optional<ev::Instant> next_update_;
    std::vector<ev::Instant>   wakes_; // asked for and not yet come
};

static_assert(ev::HandlerOf<SimClock, ev::Heartbeat> && ev::HandlerOf<SimClock, ev::WakeAt>);

} // namespace sunsim::sim
