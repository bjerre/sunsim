// The one event queue. Producers on any task post(); a single consumer
// (the app task on the target, the test on the host) runs process(), which
// calls every listener for each queued event in order. Listeners may post
// more events; those are handled on a following process() pass.

export module sunsim.ev:bus;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import :events;
import :log;

export namespace sunsim::ev {

// Something that can listen for payload T on the bus.
template <class F, class T>
concept ListenerFor = EventPayload<T> && std::invocable<F &, const T &> && std::copy_constructible<F>;

// A module that handles T: it has `void on(const T &)`, exactly that.
template <class M, class T>
concept HandlerOf = EventPayload<T> && requires { static_cast<void (M::*)(const T &)>(&M::on); };

// A module that would take T only through a conversion (`on(T)` by value,
// or `on(const Base &)` for a T derived from Base): a handler that attach()
// would miss, so it doesn't compile.
template <class M, class T>
concept TakesByConversion = !HandlerOf<M, T> && requires(M &m, const T &e) { m.on(e); };

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

    // Subscribe each module to every payload it has a handler for:
    // `void on(const T &)`. Listeners for an event run in the order they
    // were added; what they post is delivered on a later pass, so that order
    // only matters for effects outside the bus (FactoryReset: erase, then
    // reboot).
    template <class... Modules> void attach(Modules &...modules)
    {
        (attachOne(modules), ...);
    }

    // Any task.
    void enqueue(Event e)
    {
        log::debug("EV", "{}", e.name()); // the full event trace, at DEBUG
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
    template <class M> void attachOne(M &module)
    {
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (subscribe<std::variant_alternative_t<I, Payload>>(module), ...);
        }(std::make_index_sequence<std::variant_size_v<Payload>>{});
    }

    template <class T, class M> void subscribe(M &module)
    {
        static_assert(!TakesByConversion<M, T>, "a handler must take its event as `const T &`");
        if constexpr (HandlerOf<M, T>) {
            on<T>([&module](const T &e) { module.on(e); });
        }
    }

    using Listeners = std::vector<std::function<void(const Event &)>>;
    std::array<Listeners, std::variant_size_v<Payload>> listeners_; // by payload type

    mutable std::mutex              mutex_;
    mutable std::condition_variable ready_;
    std::vector<Event>              pending_; // guarded by mutex_
    std::vector<Event>              batch_;   // the consumer's, while it delivers

    Post post_ = [this](Event e) { enqueue(std::move(e)); };
};

} // namespace sunsim::ev
