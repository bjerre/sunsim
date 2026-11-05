module sunsim.sim;

import std.compat;
import sunsim.ev;
import :scheduler;
import :sim_clock;

namespace sunsim::sim {

SimClock::SimClock(ev::Post post, Scheduler &scheduler, ev::TimeReading start)
    : post_(std::move(post)), scheduler_(scheduler), start_(start)
{
}

void SimClock::start()
{
    started_ = scheduler_.now();
    scheduler_.after(std::chrono::seconds{1}, ev::Heartbeat{.at = started_ + std::chrono::seconds{1}});
}

ev::TimeReading SimClock::read() const
{
    ev::TimeReading t = start_;
    t.local += std::chrono::floor<std::chrono::seconds>(scheduler_.now() - started_);
    return t;
}

void SimClock::on(const ev::Heartbeat &e)
{
    const ev::TimeReading time = read();
    post_(ev::Tick{.at = e.at, .time = time});
    if (!next_update_) {
        next_update_ = e.at + kUpdateInterval;
    } else if (e.at >= *next_update_) {
        *next_update_ += kUpdateInterval;
        post_(ev::ProgramTick{.time = time, .transition = kUpdateInterval});
    }
    const ev::Instant next = e.at + std::chrono::seconds{1};
    scheduler_.at(next, ev::Heartbeat{.at = next});
}

void SimClock::on(const ev::WakeAt &e)
{
    if (std::ranges::contains(wakes_, e.at)) {
        return; // already asked for
    }
    wakes_.push_back(e.at);
    scheduler_.at(e.at, ev::Wake{.at = std::max(e.at, scheduler_.now())});
}

void SimClock::on(const ev::Wake &e)
{
    std::erase_if(wakes_, [&](ev::Instant w) { return w <= e.at; });
}

} // namespace sunsim::sim
