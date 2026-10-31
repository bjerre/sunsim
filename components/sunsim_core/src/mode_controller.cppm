export module sunsim.core:mode_controller;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// Which program each zone runs (ProgramHost runs it): the console can ask
// for one (ModeRequested: `tune`), and a button's presses choose its zone's,
// as its Controls say (ev::kControlsOf; by default single -> daylight,
// double -> red alert, hold -> off), posting ModeChanged. Press::Ambiguous
// (an On/Toggle only seen as an attribute change, from buttons that use the
// bound receiver endpoint) is resolved by timing: a second one within the
// double-click window is a double press, otherwise it's a single press as
// soon as the window has passed (its Timer asks for a Wake then).
class ModeController {
public:
    explicit ModeController(ev::Post post, const ev::Installation &installation,
                            std::chrono::milliseconds double_click_window = std::chrono::milliseconds{450});

    void on(const ev::ButtonPressed &press) { presses_.deliver(press, press.at); }
    void on(const ev::ModeRequested &e);
    void on(const ev::Tick &tick) { timer_.tick(tick.at); }
    void on(const ev::Wake &e) { timer_.tick(e.at); } // the double-click window, ended on time

private:
    ev::Flow run(ev::Role button);
    void     pressed(ev::Role button, ev::Press press);
    void     enter(ev::ZoneId zone, ev::Mode mode);

    ev::Post                     post_;
    const ev::Installation     &installation_;
    std::chrono::milliseconds    window_;
    ev::Timer                    timer_{[this](ev::Instant at) { post_(ev::WakeAt{at}); }};
    ev::Inbox<ev::ButtonPressed> presses_{timer_};
    ev::PerRole<ev::Flow>        flows_; // last: a run() per button, started once the rest exist
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
constexpr const char *kTag = "MODE";
} // namespace

ModeController::ModeController(ev::Post post, const ev::Installation &installation,
                               std::chrono::milliseconds double_click_window)
    : post_(std::move(post)), installation_(installation), window_(double_click_window)
{
    for (const ev::Role role : ev::kRoles) {
        if (installation_.topology.controls_of[role]) {
            flows_[role] = run(role);
        }
    }
}

ev::Flow ModeController::run(ev::Role button)
{
    const auto from = [button](const ev::ButtonPressed &p) { return p.button == button; };
    for (;;) {
        const ev::ButtonPressed press = co_await presses_.next(from);
        if (press.press != ev::Press::Ambiguous) {
            pressed(button, press.press);
            continue;
        }
        const auto second = co_await presses_.next(from, press.at + window_);
        if (!second) {
            pressed(button, ev::Press::Single); // the window passed: it was a single press
        } else if (second->press == ev::Press::Ambiguous) {
            pressed(button, ev::Press::Double); // two within the window: a double press
        } else {
            pressed(button, second->press); // a definite press wins
        }
    }
}

// What a definite press on `button` chooses, if anything.
void ModeController::pressed(ev::Role button, ev::Press press)
{
    const ev::Controls       &controls = *installation_.topology.controls_of[button];
    const std::optional<ev::Mode> mode = press == ev::Press::Double ? controls.twice
                                         : press == ev::Press::Hold ? controls.hold
                                                                     : controls.single;
    if (mode) {
        enter(*installation_.topology.zone_of[button], *mode);
    }
}

void ModeController::on(const ev::ModeRequested &e)
{
    for (const ev::ZoneId zone : installation_.zoneIds()) {
        if ((!e.zone || *e.zone == zone) && installation_.spec(zone).allows(e.mode)) {
            enter(zone, e.mode);
        }
    }
}

void ModeController::enter(ev::ZoneId zone, ev::Mode mode)
{
    const std::string_view name = installation_.spec(zone).name;
    switch (mode) {
    case ev::Mode::Daylight:
        ev::log::info(kTag, "{}: resuming daylight", name);
        break;
    case ev::Mode::Off:
        ev::log::info(kTag, "{}: manual off (press again to resume)", name);
        break;
    case ev::Mode::RedAlert:
        ev::log::warn(kTag, "{}: RED ALERT", name);
        break;
    case ev::Mode::Tuning:
        ev::log::info(kTag, "{}: tuning, every light steady at its minimum level (`tune`; press to resume)", name);
        break;
    }
    post_(ev::ModeChanged{.zone = zone, .mode = mode});
}

} // namespace sunsim::core
