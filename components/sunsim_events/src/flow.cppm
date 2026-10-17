// Coroutines for behavior that happens in steps over time ("send this,
// wait for the reply or 15 s, try again"), written top to bottom instead of
// spread over handlers, flags and counters. Everything runs on the app
// task, so nothing here locks.
//
//   class Registry {
//       void on(const Reply &r) { replies_.deliver(r); }
//       void on(const Tick &t)  { timer_.tick(t.at); }
//
//       Flow ask(Role role) {
//           for (;;) {
//               post_(Request{role});
//               if (const auto reply = co_await replies_.next(forRole(role), timer_.now() + 15s)) {
//                   co_return use(*reply);
//               }
//           }
//       }
//
//       Timer        timer_;
//       Inbox<Reply> replies_{timer_};
//       Flow         asking_; // = ask(role); reassigning or destroying it cancels the flow
//   };

export module sunsim.ev:flow;

import std.compat;
import :types;

export namespace sunsim::ev {

// A coroutine that starts at once and runs until it returns or its owner
// destroys it, which cancels it wherever it's waiting. Nobody awaits it.
class Flow {
public:
    struct promise_type {
        Flow                get_return_object() noexcept { return Flow{Handle::from_promise(*this)}; }
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; } // the owner frees it
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::abort(); } // exceptions are off
    };

    Flow() = default; // no flow
    Flow(Flow &&other) noexcept : handle_(std::exchange(other.handle_, {})) {}
    Flow &operator=(Flow &&other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }
    ~Flow() { reset(); }

private:
    using Handle = std::coroutine_handle<promise_type>;
    explicit Flow(Handle handle) noexcept : handle_(handle) {}

    void reset() noexcept
    {
        if (handle_) {
            std::exchange(handle_, {}).destroy();
        }
    }

    Handle handle_;
};

class Timer;

namespace detail {
// A suspended co_await with a deadline, which its Timer expires.
class Timed {
public:
    Timed(const Timed &)            = delete;
    Timed &operator=(const Timed &) = delete;

    virtual void expire() = 0; // the deadline came first: resume the flow

protected:
    Timed(Timer &timer, Instant deadline) noexcept : timer_(timer), deadline_(deadline) {}
    ~Timed();

    void startTimer();
    void stopTimer() noexcept;

    Timer  &timer_;
    Instant deadline_;

private:
    friend class sunsim::ev::Timer;
    bool queued_ = false;
};
} // namespace detail

// Time for flows. The owning module calls tick() with each Tick's time, and
// timestamped events move it too (Inbox::deliver); due waits resume then.
// Time only moves forward. Ticks come once a second; for a wait that must
// end sooner, give the Timer `wake`: it's called with each new earliest
// deadline, for the owner to post WakeAt, and tick() it on the Wake.
class Timer {
public:
    Timer() = default;
    explicit Timer(std::function<void(Instant)> wake) : wake_(std::move(wake)) {}
    ~Timer();

    [[nodiscard]] Instant now() const noexcept { return now_; }

    // co_await until(t) / after(d): resumes on the first tick at or after the
    // deadline, or at once if it has already passed.
    [[nodiscard]] auto until(Instant deadline) noexcept { return Sleep{*this, deadline}; }
    [[nodiscard]] auto after(BootClock::duration delay) noexcept { return until(now_ + delay); }

    void tick(Instant now)
    {
        now_ = std::max(now_, now);
        // One at a time: expiring resumes a flow, which may start or cancel others.
        for (;;) {
            const auto due = std::ranges::find_if(timed_, [&](const detail::Timed *t) { return t->deadline_ <= now_; });
            if (due == timed_.end()) {
                break;
            }
            detail::Timed *timed = *due;
            timed_.erase(due);
            timed->queued_ = false;
            timed->expire();
        }
        announce();
    }

private:
    friend class detail::Timed;

    // The earliest deadline to `wake`, if it's new.
    void announce()
    {
        if (!wake_ || timed_.empty()) {
            return;
        }
        const Instant earliest = (*std::ranges::min_element(timed_, {}, &detail::Timed::deadline_))->deadline_;
        if (earliest != announced_) {
            announced_ = earliest;
            wake_(earliest);
        }
    }

    class Sleep final : public detail::Timed {
    public:
        Sleep(Timer &timer, Instant deadline) noexcept : Timed(timer, deadline) {}

        [[nodiscard]] bool await_ready() const noexcept { return deadline_ <= timer_.now(); }
        void               await_suspend(std::coroutine_handle<> flow)
        {
            flow_ = flow;
            startTimer();
        }
        void await_resume() const noexcept {}

        void expire() override { flow_.resume(); }

    private:
        std::coroutine_handle<> flow_;
    };

    Instant                      now_{};
    std::vector<detail::Timed *> timed_;
    std::function<void(Instant)> wake_;
    Instant                      announced_{};
};

// Not inline: sunsim.ev's object file has the one copy. (Inline, GCC 15
// left a module that destroys a Timer linking to vector internals nobody
// emitted.)
Timer::~Timer() = default;

inline detail::Timed::~Timed()
{
    stopTimer(); // the flow was destroyed while waiting
}

inline void detail::Timed::startTimer()
{
    timer_.timed_.push_back(this);
    queued_ = true;
    timer_.announce();
}

inline void detail::Timed::stopTimer() noexcept
{
    if (queued_) {
        std::erase(timer_.timed_, this);
        queued_ = false;
    }
}

// Hands each T to the flows waiting for one (each with its own filter and
// optional deadline). The owning module calls deliver() from its handler
// for T. A T nobody is waiting for is dropped.
template <class T> class Inbox {
public:
    explicit Inbox(Timer &timer) noexcept : timer_(timer) {}
    Inbox(const Inbox &)            = delete;
    Inbox &operator=(const Inbox &) = delete;

    // co_await next() / next(filter): the next T (that passes the filter).
    [[nodiscard]] auto next() { return Wait<false>{*this, any, Instant::max()}; }
    template <std::predicate<const T &> F> [[nodiscard]] auto next(F filter)
    {
        return Wait<false>{*this, std::move(filter), Instant::max()};
    }

    // co_await next(deadline) / next(filter, deadline): the same, or nullopt
    // once the deadline has passed.
    [[nodiscard]] auto next(Instant deadline) { return Wait<true>{*this, any, deadline}; }
    template <std::predicate<const T &> F> [[nodiscard]] auto next(F filter, Instant deadline)
    {
        return Wait<true>{*this, std::move(filter), deadline};
    }

    // `at`, if given, is when it happened: the timer moves on to it first, so
    // waits whose deadline had passed by then time out (and a flow that then
    // waits again here can take this one).
    void deliver(const T &value, std::optional<Instant> at = std::nullopt)
    {
        if (at) {
            timer_.tick(*at);
        }
        // Only those waiting now: a resumed flow that waits again gets the next one.
        const uint32_t last = serial_;
        while (Waiting *w = find([&](const Waiting *w) { return w->serial_ <= last && w->filter_(value); })) {
            w->take(value);
        }
    }

private:
    using Filter                = std::function<bool(const T &)>;
    static constexpr auto any   = [](const T &) { return true; };

    class Waiting : public detail::Timed {
    public:
        Waiting(Inbox &inbox, Filter filter, Instant deadline)
            : Timed(inbox.timer_, deadline), inbox_(inbox), filter_(std::move(filter))
        {
        }
        ~Waiting() { leave(); }

        [[nodiscard]] bool await_ready() const noexcept { return deadline_ <= timer_.now(); }
        void               await_suspend(std::coroutine_handle<> flow)
        {
            flow_   = flow;
            serial_ = ++inbox_.serial_;
            inbox_.waiting_.push_back(this);
            if (deadline_ != Instant::max()) {
                startTimer();
            }
        }

        void expire() override
        {
            leave();
            flow_.resume();
        }
        void take(const T &value)
        {
            value_ = value;
            leave();
            flow_.resume();
        }

    protected:
        std::optional<T> value_;

    private:
        friend class Inbox;

        void leave() noexcept
        {
            std::erase(inbox_.waiting_, this);
            stopTimer();
        }

        Inbox                  &inbox_;
        Filter                  filter_;
        std::coroutine_handle<> flow_;
        uint32_t                serial_ = 0;
    };

    template <bool kTimed> class Wait final : public Waiting {
    public:
        using Waiting::Waiting;

        auto await_resume()
        {
            if constexpr (kTimed) {
                return std::move(this->value_);
            } else {
                return std::move(*this->value_);
            }
        }
    };

    template <class Pred> Waiting *find(Pred pred) const
    {
        const auto it = std::ranges::find_if(waiting_, pred);
        return it == waiting_.end() ? nullptr : *it;
    }

    Timer                 &timer_;
    std::vector<Waiting *> waiting_;
    uint32_t               serial_ = 0;
};

} // namespace sunsim::ev
