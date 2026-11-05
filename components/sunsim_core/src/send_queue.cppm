export module sunsim.core:send_queue;

import std.compat;
import sunsim.ev;

export namespace sunsim::core {

// A Zigbee group: lights the bulbs themselves know as its members, so one
// command to its id reaches them all at once.
struct LightGroup {
    uint16_t    id = 0;
    ev::RoleSet members;
};

// Between LightOutput and the radio: holds each light's commands (SetLight)
// until the ZigbeeAdapter has room for one (a SendSlot, a few ms apart while
// any are waiting: SendsWaiting asks for the next), so a burst for every
// light goes out paced instead of all at once. A newer SetLight for a light
// merges over the one waiting. Each slot sends one ZCL command (SendLight):
// to the largest group whose lights all wait for exactly that command (the
// same step, value and transition), else to the light alone. The groups
// fit whatever the lights are showing: red alert's whole room, an alert's
// pulse on two wall lights while the others follow the sun. A light's
// commands keep SetLight's order (On, a saturated color, the level, a
// white or xy color, Off), and the lights take turns, the longest waiting
// first. A group command is a broadcast, which the network allows few of:
// past kMaxGroupSends in kGroupSendWindow, they go to each light instead.
class SendQueue {
public:
    static constexpr std::size_t          kMaxGroupSends = 8;
    static constexpr std::chrono::seconds kGroupSendWindow{9};

    explicit SendQueue(ev::Post post, std::vector<LightGroup> groups = {});

    void on(const ev::SetLight &e);
    void on(const ev::SendSlot &e);
    void on(const ev::RoleCleared &e) { drop(e.role); }
    void on(const ev::ReachabilityChanged &e);

private:
    [[nodiscard]] const LightGroup *largestWithin(const ev::RoleSet &lights, ev::Role first) const noexcept;
    void                            drop(ev::Role role);
    void                            ask();

    ev::Post                                 post_;
    std::vector<LightGroup>                  groups_;
    ev::PerRole<std::optional<ev::SetLight>> waiting_{};
    std::vector<ev::Role>                    turns_;       // the lights waiting, the next to send first
    std::deque<ev::Instant>                  group_sends_; // within the window, oldest first
    bool                                     asked_ = false; // SendsWaiting posted, its SendSlot not yet come
};

} // namespace sunsim::core

namespace sunsim::core {

namespace {
// A light's commands, in the order they go out.
enum class Step : uint8_t { On, HueSat, Level, Color, Off };

std::optional<Step> firstStep(const ev::SetLight &s) noexcept
{
    if (s.power == true) {
        return Step::On;
    }
    if (s.color && std::holds_alternative<ev::HueSat>(*s.color)) {
        return Step::HueSat;
    }
    if (s.level) {
        return Step::Level;
    }
    if (s.color) {
        return Step::Color;
    }
    if (s.power == false) {
        return Step::Off;
    }
    return std::nullopt;
}

// The command for `step`, to nowhere yet.
ev::SendLight commandOf(const ev::SetLight &s, Step step) noexcept
{
    ev::SendLight c;
    switch (step) {
    case Step::On:
    case Step::Off:
        c.power = s.power;
        break;
    case Step::Level:
        c.level      = s.level;
        c.transition = s.level_transition;
        break;
    case Step::HueSat:
    case Step::Color:
        c.color      = s.color;
        c.transition = s.color_transition;
        break;
    }
    return c;
}

void sent(ev::SetLight &s, Step step) noexcept
{
    switch (step) {
    case Step::On:
    case Step::Off:
        s.power.reset();
        break;
    case Step::Level:
        s.level.reset();
        break;
    case Step::HueSat:
    case Step::Color:
        s.color.reset();
        break;
    }
}

bool sameCommand(const ev::SendLight &a, const ev::SendLight &b) noexcept
{
    return a.power == b.power && a.level == b.level && a.color == b.color && a.transition == b.transition;
}
} // namespace

SendQueue::SendQueue(ev::Post post, std::vector<LightGroup> groups) : post_(std::move(post)), groups_(std::move(groups))
{
}

void SendQueue::on(const ev::SetLight &e)
{
    if (!ev::isLight(e.role) || !firstStep(e)) {
        return;
    }
    std::optional<ev::SetLight> &w = waiting_[e.role];
    if (!w) {
        turns_.push_back(e.role);
        w = e;
    } else if (e.power == false) {
        w = e; // off: what was waiting doesn't matter any more
    } else {
        w->short_addr = e.short_addr;
        w->endpoint   = e.endpoint;
        if (e.power) {
            w->power = e.power;
        }
        if (e.level) {
            w->level            = e.level;
            w->level_transition = e.level_transition;
        }
        if (e.color) {
            w->color            = e.color;
            w->color_transition = e.color_transition;
        }
    }
    ask();
}

void SendQueue::on(const ev::SendSlot &e)
{
    asked_ = false;
    if (turns_.empty()) {
        return;
    }
    while (!group_sends_.empty() && e.at - group_sends_.front() >= kGroupSendWindow) {
        group_sends_.pop_front();
    }

    const ev::Role      first = turns_.front();
    const ev::SetLight &head  = *waiting_[first];
    const Step          step  = *firstStep(head);
    ev::SendLight       send  = commandOf(head, step);
    send.backlog              = static_cast<uint8_t>(std::min<std::size_t>(turns_.size(), 255));

    // Every light waiting for exactly this command.
    ev::RoleSet same;
    for (const ev::Role r : turns_) {
        if (firstStep(*waiting_[r]) == step && sameCommand(commandOf(*waiting_[r], step), send)) {
            same.set(ev::slot(r));
        }
    }
    ev::RoleSet to;
    const LightGroup *group = group_sends_.size() < kMaxGroupSends ? largestWithin(same, first) : nullptr;
    if (group) {
        send.to = {.address = group->id, .group = true};
        to      = group->members;
        group_sends_.push_back(e.at);
    } else {
        send.to = {.address = head.short_addr, .endpoint = head.endpoint};
        to.set(ev::slot(first));
    }
    post_(send);

    // Each light it went to: on to its next command, at the back of the line.
    std::vector<ev::Role> later;
    std::ranges::copy_if(turns_, std::back_inserter(later), [&](ev::Role r) { return !ev::contains(to, r); });
    for (const ev::Role r : turns_) {
        if (ev::contains(to, r)) {
            sent(*waiting_[r], step);
            if (firstStep(*waiting_[r])) {
                later.push_back(r);
            } else {
                waiting_[r].reset();
            }
        }
    }
    turns_ = std::move(later);
    ask();
}

void SendQueue::on(const ev::ReachabilityChanged &e)
{
    if (!e.reachable) {
        drop(e.role); // LightOutput sends it everything when it's back
    }
}

// The largest group with `first` whose lights are all in `lights`: none
// left out, so none gets a command that isn't its own.
const LightGroup *SendQueue::largestWithin(const ev::RoleSet &lights, ev::Role first) const noexcept
{
    const LightGroup *best = nullptr;
    for (const LightGroup &g : groups_) {
        if (g.members.count() >= 2 && ev::contains(g.members, first) && (g.members & ~lights).none() &&
            (!best || g.members.count() > best->members.count())) {
            best = &g;
        }
    }
    return best;
}

void SendQueue::drop(ev::Role role)
{
    if (ev::isLight(role)) {
        waiting_[role].reset();
        std::erase(turns_, role);
    }
}

void SendQueue::ask()
{
    if (!asked_ && !turns_.empty()) {
        asked_ = true;
        post_(ev::SendsWaiting{});
    }
}

} // namespace sunsim::core
