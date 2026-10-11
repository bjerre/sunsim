// The one event queue. Producers on any task post(); a single consumer
// (the app task on the target, the test on the host) runs process(), which
// calls every listener for each queued event in order. Listeners may post
// more events; those are handled on a following process() pass.

module;

#include "sunsim/ev/log.h"

export module sunsim.ev:bus;

import std.compat;
import :events;

export namespace sunsim::ev {

// Something that can listen for payload T on the bus.
template <class F, class T>
concept ListenerFor = EventPayload<T> && std::invocable<F &, const T &> && std::copy_constructible<F>;

// process() takes everything queued so far under the lock, then delivers it
// without holding it, so posting never waits on a listener. Listeners are
// added while wiring, before anything is processed, and never removed.
class EventBus {
public:
    EventBus() = default;

    EventBus(const EventBus &)            = delete;
    EventBus &operator=(const EventBus &) = delete;

    // Listen for payload type T: `bus.on<Tick>([&](const Tick &t) { ... });`
    template <EventPayload T> void on(ListenerFor<T> auto listener)
    {
        listeners_[kTypeId<T>].emplace_back(
            [listener = std::move(listener)](const Event &e) mutable { listener(e.as<T>()); });
    }

    // Any task.
    void enqueue(Event e)
    {
        SUNSIM_LOGD("EV", "%s", e.name()); // full event trace at DEBUG
        {
            const std::scoped_lock lock{mutex_};
            pending_.push_back(std::move(e));
        }
        ready_.notify_one();
    }

    [[nodiscard]] const Post &post() const noexcept { return post_; }

    // The consumer: block until something is queued.
    void wait() const
    {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return !pending_.empty(); });
    }

    // The consumer: deliver everything queued so far; false if there was nothing.
    bool process()
    {
        {
            const std::scoped_lock lock{mutex_};
            batch_.swap(pending_); // both keep their capacity: no allocating once warmed up
        }
        for (const Event &e : batch_) {
            for (const auto &listener : listeners_[e.type()]) {
                listener(e);
            }
        }
        const bool any = !batch_.empty();
        batch_.clear();
        return any;
    }

    // Process until no events remain, including ones posted while processing.
    void drain()
    {
        while (process()) {
        }
    }

private:
    using Listeners = std::vector<std::function<void(const Event &)>>;
    std::array<Listeners, std::variant_size_v<Payload>> listeners_; // by payload type

    mutable std::mutex              mutex_;
    mutable std::condition_variable ready_;
    std::vector<Event>              pending_; // guarded by mutex_
    std::vector<Event>              batch_;   // the consumer's, while it delivers

    Post post_ = [this](Event e) { enqueue(std::move(e)); };
};

} // namespace sunsim::ev
