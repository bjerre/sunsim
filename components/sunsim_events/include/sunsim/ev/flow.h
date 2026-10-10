#pragma once

// Coroutines for multi-step behaviour that would otherwise be spread over
// event handlers and state flags ("wait for this, then that, or give up
// after N ms"). Everything runs on the app task, so nothing here locks.
//
//   class X {
//       void onPress(const Press &p) { presses_.deliver(p, p.at_us); }
//       void onTick(const Tick &t)   { presses_.tick(t.uptime_us); }
//       Flow run() {
//           for (;;) {
//               const Press first = co_await presses_.next();
//               const std::optional<Press> second = co_await presses_.next(first.at_us + kWindowUs);
//               ...
//           }
//       }
//       Mailbox<Press> presses_;
//       Flow           flow_ = run(); // last member: starts once the rest exist
//   };

#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>

namespace sunsim::ev {

// A coroutine that starts at once and runs until it returns or its owner
// destroys it (which cancels it wherever it's waiting). Nobody awaits it.
class Flow {
public:
    struct promise_type {
        Flow get_return_object() noexcept { return Flow{Handle::from_promise(*this)}; }
        std::suspend_never  initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; } // the owner frees it
        void                return_void() noexcept {}
        void                unhandled_exception() noexcept { std::abort(); } // exceptions are off
    };

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
            handle_.destroy();
            handle_ = {};
        }
    }

    Handle handle_;
};

// Hands the next T to the one Flow waiting for it. The owning module calls
// deliver() from its event handler and tick() from its Tick handler; times
// are uptime in microseconds. A T that arrives while nobody waits is dropped.
template <class T> class Mailbox {
public:
    // co_await next(): the next T.
    auto next() noexcept { return Awaiter<false>{*this, kNoDeadline}; }

    // co_await next(deadline_us): the next T, or nullopt once deadline_us
    // has passed (seen as a later tick(), or a T stamped after it).
    auto next(int64_t deadline_us) noexcept { return Awaiter<true>{*this, deadline_us}; }

    void deliver(const T &value, int64_t at_us)
    {
        if (waiting_ && at_us > deadline_us_) {
            resume(std::nullopt); // too late for that wait: time it out first...
        }
        if (waiting_) {
            resume(value); // ...then it's the next wait's
        }
    }

    void tick(int64_t now_us)
    {
        if (waiting_ && now_us > deadline_us_) {
            resume(std::nullopt);
        }
    }

private:
    static constexpr int64_t kNoDeadline = INT64_MAX;

    template <bool kTimed> struct Awaiter {
        Mailbox &box;
        int64_t  deadline_us;

        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) noexcept
        {
            box.waiting_     = handle;
            box.deadline_us_ = deadline_us;
        }
        auto await_resume() noexcept
        {
            if constexpr (kTimed) {
                return std::exchange(box.value_, std::nullopt);
            } else {
                return *std::exchange(box.value_, std::nullopt);
            }
        }
    };

    void resume(std::optional<T> value)
    {
        value_ = std::move(value);
        // Cleared first: the flow usually waits again before resume() returns.
        std::exchange(waiting_, {}).resume();
    }

    std::coroutine_handle<> waiting_;
    int64_t                 deadline_us_ = kNoDeadline;
    std::optional<T>        value_;
};

} // namespace sunsim::ev
