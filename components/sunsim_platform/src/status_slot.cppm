export module sunsim.platform:status_slot;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::platform {

// The latest StatusSnapshot, for readers outside the app task (console
// commands, the display task).
class StatusSlot {
public:
    void on(const ev::StatusChanged &e)
    {
        const std::scoped_lock lock{mutex_};
        snapshot_ = *e.snapshot;
    }

    // Empty until the first status update (about a second after the app starts).
    [[nodiscard]] std::optional<ev::StatusSnapshot> get() const
    {
        const std::scoped_lock lock{mutex_};
        return snapshot_;
    }

private:
    mutable std::mutex                mutex_;
    std::optional<ev::StatusSnapshot> snapshot_;
};

} // namespace sunsim::platform
