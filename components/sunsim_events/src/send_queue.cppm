// Light commands waiting for the radio (ZigbeeAdapter sends them paced): one
// per light, handed out in turn. Pure, so it's host-tested.

export module sunsim.ev:send_queue;

import std.compat;
import :events;
import :types;

export namespace sunsim::ev {

// One pending SetLight per light. A newer one is merged over the one still
// waiting - each field the latest, with its transition - so under load the
// in-between states are dropped instead of piling up. pop() takes the lights
// in turn (round-robin, in slot order), so one busy light can't starve the
// rest.
class SendQueue {
public:
    void push(const SetLight &s);

    // The next light's commands, or nullopt if none are waiting.
    [[nodiscard]] std::optional<SetLight> pop();

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // The most lights waiting at once since the last call (for the send report).
    [[nodiscard]] std::size_t takeHighWater() noexcept { return std::exchange(high_water_, size_); }

private:
    PerRole<std::optional<SetLight>> pending_{};
    std::size_t                      next_       = 0; // the slot pop() looks at first
    std::size_t                      size_       = 0;
    std::size_t                      high_water_ = 0;
};

} // namespace sunsim::ev

namespace sunsim::ev {

namespace {
// `newer` over `older`, for the same light: what takes it to newer's state.
// Off says it all (the level and color go again with the next On: see
// core::commandsFor); otherwise each field is the newer one if it has it.
SetLight merged(const SetLight &older, const SetLight &newer)
{
    if (newer.power == false) {
        return newer;
    }
    SetLight s = newer;
    if (!s.power) {
        s.power = older.power;
    }
    if (!s.level) {
        s.level            = older.level;
        s.level_transition = older.level_transition;
    }
    if (!s.color) {
        s.color            = older.color;
        s.color_transition = older.color_transition;
    }
    return s;
}
} // namespace

void SendQueue::push(const SetLight &s)
{
    auto &pending = pending_[s.role];
    if (pending) {
        pending = merged(*pending, s);
        return;
    }
    pending    = s;
    size_      += 1;
    high_water_ = std::max(high_water_, size_);
}

std::optional<SetLight> SendQueue::pop()
{
    for (std::size_t i = 0; i < kRoleCount; ++i) {
        const std::size_t slot = (next_ + i) % kRoleCount;
        if (auto &pending = pending_.items[slot]) {
            next_ = slot + 1;
            size_ -= 1;
            return std::exchange(pending, std::nullopt);
        }
    }
    return std::nullopt;
}

} // namespace sunsim::ev
