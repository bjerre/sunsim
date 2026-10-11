// The one event queue. Producers on any task post(); a single consumer
// (the app task on the target, the test on the host) runs process(), which
// calls every listener for each queued event in order. Listeners may post
// more events; those are handled on a following process() pass.

module;

#include <eventpp/eventqueue.h>

#include "sunsim/ev/log.h"

export module sunsim.ev:bus;

import std.compat;
import :events;

export namespace sunsim::ev {

class EventBus {
public:
    EventBus() : post_([this](Event e) { enqueue(std::move(e)); }) {}

    EventBus(const EventBus &)            = delete;
    EventBus &operator=(const EventBus &) = delete;

    // Listen for payload type T: `bus.on<Tick>([&](const Tick &t) { ... });`
    template <class T, class F> void on(F listener)
    {
        queue_.appendListener(kTypeId<T>, [listener = std::move(listener)](const Event &e) mutable {
            listener(e.as<T>());
        });
    }

    void enqueue(Event e)
    {
        SUNSIM_LOGD("EV", "%s", e.name()); // full event trace at DEBUG
        queue_.enqueue(std::move(e));
    }

    [[nodiscard]] const Post &post() const noexcept { return post_; }

    void wait() const { queue_.wait(); }
    bool process() { return queue_.process(); }

    // Process until no events remain, including ones posted while processing.
    void drain()
    {
        while (queue_.process()) {
        }
    }

private:
    struct Policy {
        static std::size_t getEvent(const Event &e) { return e.type(); }
    };
    eventpp::EventQueue<std::size_t, void(const Event &), Policy> queue_;
    Post                                                           post_;
};

} // namespace sunsim::ev
