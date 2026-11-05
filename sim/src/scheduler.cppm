export module sunsim.sim:scheduler;

import std.compat;
import sunsim.ev;

export namespace sunsim::sim {

// Virtual time for a simulated run: events due at an instant, posted to the
// bus when the run gets there, with everything they set off handled before
// time moves on. Nothing waits: a day runs as fast as its events can be
// handled, and the same way every time.
class Scheduler {
public:
    explicit Scheduler(ev::EventBus &bus) : bus_(bus) {}

    Scheduler(const Scheduler &)            = delete;
    Scheduler &operator=(const Scheduler &) = delete;

    [[nodiscard]] ev::Instant now() const noexcept { return now_; }

    // `e` at `when` (at once, if that's past). Those due at the same instant
    // go in the order they were scheduled.
    void at(ev::Instant when, ev::Event e);
    void after(ev::BootClock::duration delay, ev::Event e) { at(now_ + delay, std::move(e)); }

    // Everything due by `until`, in time order; then the time is `until`.
    void runUntil(ev::Instant until);
    void runFor(ev::BootClock::duration duration) { runUntil(now_ + duration); }

private:
    struct Due {
        ev::Instant at{};
        uint64_t    order = 0;
        ev::Event   event;
    };
    // For the heap: the soonest on top.
    static bool later(const Due &a, const Due &b) noexcept
    {
        return a.at != b.at ? a.at > b.at : a.order > b.order;
    }

    ev::EventBus    &bus_;
    ev::Instant      now_{};
    uint64_t         scheduled_ = 0;
    std::vector<Due> due_; // a heap (later())
};

} // namespace sunsim::sim

namespace sunsim::sim {

void Scheduler::at(ev::Instant when, ev::Event e)
{
    due_.push_back({.at = std::max(when, now_), .order = scheduled_++, .event = std::move(e)});
    std::ranges::push_heap(due_, later);
}

void Scheduler::runUntil(ev::Instant until)
{
    bus_.drain();
    while (!due_.empty() && due_.front().at <= until) {
        std::ranges::pop_heap(due_, later);
        Due next = std::move(due_.back());
        due_.pop_back();
        now_ = next.at;
        bus_.post()(std::move(next.event));
        bus_.drain();
    }
    now_ = std::max(now_, until);
}

} // namespace sunsim::sim
