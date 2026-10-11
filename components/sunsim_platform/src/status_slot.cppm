export module sunsim.platform:status_slot;

import std.compat;
import sunsim.ev;

export namespace sunsim::platform {

// The latest StatusSnapshot, for readers outside the app task (console
// commands, the display task). Written by a StatusChanged listener.
class StatusSlot {
public:
    void set(const ev::StatusSnapshot &snapshot)
    {
        std::lock_guard lock(mutex_);
        snapshot_ = snapshot;
    }

    // Empty until the first status update (about a second after the app starts).
    [[nodiscard]] std::optional<ev::StatusSnapshot> get() const
    {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

private:
    mutable std::mutex                mutex_;
    std::optional<ev::StatusSnapshot> snapshot_;
};

} // namespace sunsim::platform
