// The paced send queue: SetLight in, one ZCL command per slot out, to a
// Zigbee group where every light in it waits for the same command.

import std.compat;
import sunsim.ev;
import sunsim.core;
import sunsim.test;

using namespace sunsim;
using namespace sunsim::ev;
using namespace sunsim::testing;
using namespace std::chrono_literals;

namespace {
constexpr Role kWallD{Group::Wall, 3};

RoleSet setOf(std::initializer_list<Role> roles)
{
    RoleSet set;
    for (const Role r : roles) {
        set.set(slot(r));
    }
    return set;
}

// The groups section 1 would derive for this room: the whole room, the
// wall, the alert lights, and the rest of the wall and the room without them.
constexpr uint16_t kRoomGroup  = 0x5301;
constexpr uint16_t kWallGroup  = 0x5302;
constexpr uint16_t kAlertGroup = 0x5303;
std::vector<core::LightGroup> roomGroups()
{
    return {
        {kRoomGroup, setOf({kWallA, kWallB, kWallC, kWallD, kWallE, kWork})},
        {kWallGroup, setOf({kWallA, kWallB, kWallC, kWallD, kWallE})},
        {kAlertGroup, setOf({kWallB, kWallD})},
        {0x5304, setOf({kWallA, kWallC, kWallE})},
        {0x5305, setOf({kWallA, kWallC, kWallE, kWork})},
    };
}

// Each light's short address in the harness: its slot, plus one.
uint16_t addressOf(Role role)
{
    return static_cast<uint16_t>(slot(role) + 1);
}

// A SendQueue on its own, given its slots by hand.
struct Sends {
    std::vector<SendLight> sent;
    bool                   waiting = false; // a SendsWaiting not yet answered
    Instant                now{};
    core::SendQueue        queue;

    explicit Sends(std::vector<core::LightGroup> groups = {})
        : queue([this](Event e) {
              if (const auto *s = std::get_if<SendLight>(&e.data)) {
                  sent.push_back(*s);
              } else if (std::holds_alternative<SendsWaiting>(e.data)) {
                  waiting = true;
              }
          }, std::move(groups))
    {
    }

    // Slots, 25 ms apart, until nothing's waiting.
    void drain()
    {
        while (std::exchange(waiting, false)) {
            now += 25ms;
            queue.on(SendSlot{.at = now});
        }
    }

    // What the core posted, through the queue.
    void take(const Harness &h)
    {
        for (const SetLight &s : h.all<SetLight>()) {
            queue.on(s);
        }
        drain();
    }

    [[nodiscard]] std::vector<SendLight> to(uint16_t group) const
    {
        std::vector<SendLight> out;
        std::ranges::copy_if(sent, std::back_inserter(out),
                             [&](const SendLight &s) { return s.to.group && s.to.address == group; });
        return out;
    }
    [[nodiscard]] std::vector<SendLight> to(Role light) const
    {
        std::vector<SendLight> out;
        std::ranges::copy_if(sent, std::back_inserter(out),
                             [&](const SendLight &s) { return !s.to.group && s.to.address == addressOf(light); });
        return out;
    }
};

SetLight setLight(Role role, std::optional<bool> power, std::optional<uint8_t> level, std::optional<Color> color = {})
{
    return {.role = role, .short_addr = addressOf(role), .endpoint = 11, .power = power, .level = level, .color = color};
}

// Every light paired, each at addressOf(), the daylight program running.
void pairRoom(Harness &h)
{
    h.tick();
    for (const Role role : lights()) {
        h.pairBulb(role, 0xA0 + slot(role), addressOf(role));
    }
    h.clearLog();
}

AlertsUpdated watch(Instant at)
{
    AlertsUpdated a{.at = at};
    a.alerts.items[0] = {.level = AlertLevel::Watch, .event = {'X'}, .until = at + 2h};
    a.alerts.count    = 1;
    return a;
}
} // namespace

const suite<"sends"> sendsTests = [] {
    "commands wait for a slot each; a light's keep their order, the lights take turns"_test = [] {
        Sends s;
        s.queue.on(setLight(kWallA, true, 200, HueSat{0, 254}));
        s.queue.on(setLight(kWallB, {}, 100));
        expect(s.waiting && s.sent.empty()); // nothing until the first slot
        s.drain();
        expect(that % s.sent.size() == 4u);
        if (s.sent.size() == 4) {
            expect(s.sent[0].to.address == addressOf(kWallA) && s.sent[0].power == true);
            expect(s.sent[1].to.address == addressOf(kWallB) && s.sent[1].level == 100);
            expect(s.sent[2].to.address == addressOf(kWallA) && s.sent[2].color == Color{HueSat{0, 254}});
            expect(s.sent[3].to.address == addressOf(kWallA) && s.sent[3].level == 200);
            expect(s.sent[0].backlog == 2 && s.sent[3].backlog == 1);
        }
        expect(!s.waiting);
    };

    "a whiter color goes after the level, a saturated one before it"_test = [] {
        Sends s;
        s.queue.on(setLight(kWallA, {}, 50, Mireds{300}));
        s.drain();
        expect(s.sent.size() == 2 && s.sent[0].level && s.sent[1].color);
    };

    "a newer command merges over the waiting one; Off replaces it"_test = [] {
        Sends s;
        s.queue.on(setLight(kWallA, {}, 100));
        s.queue.on(setLight(kWallA, {}, 200, Mireds{250}));
        s.drain();
        expect(s.sent.size() == 2 && s.sent[0].level == 200 && s.sent[1].color == Color{Mireds{250}});

        s.sent.clear();
        s.queue.on(setLight(kWallA, true, 100));
        s.queue.on(setLight(kWallA, false, {}));
        s.drain();
        expect(s.sent.size() == 1 && s.sent[0].power == false);
    };

    "a light out of reach or unpaired loses what was waiting for it"_test = [] {
        Sends s;
        s.queue.on(setLight(kWallA, {}, 100));
        s.queue.on(setLight(kWallB, {}, 100));
        s.queue.on(setLight(kWork, {}, 100));
        s.queue.on(ReachabilityChanged{.role = kWallA, .reachable = false});
        s.queue.on(RoleCleared{.role = kWork});
        s.drain();
        expect(s.sent.size() == 1 && s.sent[0].to.address == addressOf(kWallB));
    };

    "red alert: the whole room in one group command per step"_test = [] {
        Harness h;
        pairRoom(h);
        h.post(ButtonPressed{.press = Press::Double, .source = Source::Zigbee, .at = h.now});
        Sends s{roomGroups()};
        s.take(h);
        expect(that % s.sent.size() == 2u); // the red, then the level
        expect(s.to(kRoomGroup).size() == 2);
        expect(s.sent.size() == 2 && s.sent[0].color == Color{HueSat{0, 254}} && s.sent[1].level);

        h.clearLog();
        s.sent.clear();
        h.tick(); // the beat: only the level moves
        s.take(h);
        expect(s.sent.size() == 1 && s.to(kRoomGroup).size() == 1 && s.sent[0].level);
    };

    "red alert with a light out of reach: the largest groups without it"_test = [] {
        Harness h;
        pairRoom(h);
        h.post(ReachabilityChanged{.role = kWallC, .reachable = false});
        h.post(ButtonPressed{.press = Press::Double, .source = Source::Zigbee, .at = h.now});
        Sends s{roomGroups()};
        s.queue.on(ReachabilityChanged{.role = kWallC, .reachable = false});
        s.take(h);
        expect(s.to(kRoomGroup).empty() && s.to(kWallGroup).empty() && s.to(kWallC).empty());
        expect(s.to(kAlertGroup).size() == 2); // B and D together
        for (const Role role : {kWallA, kWallE, kWork}) {
            expect(s.to(role).size() == 2);
        }
    };

    "a weather alert: its two lights share a group, the rest go on with the sun"_test = [] {
        Harness h;
        pairRoom(h);
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        h.post(watch(h.now));
        Sends s{roomGroups()};
        s.take(h);
        s.sent.clear();
        h.clearLog();
        h.tick(3); // the pulse fades, and the program goes on
        h.post(ProgramTick{.time = noon(), .transition = 60s});
        s.take(h);
        expect(!s.to(kAlertGroup).empty());
        expect(s.to(kWallB).empty() && s.to(kWallD).empty()); // never one at a time
        expect(s.to(kRoomGroup).empty() && s.to(kWallGroup).empty()); // never over the pulse
        for (const SendLight &send : s.to(kAlertGroup)) {
            expect(!send.color || std::holds_alternative<HueSat>(*send.color)); // the watch's orange
        }
    };

    "the lights that don't share a group get theirs one at a time"_test = [] {
        Sends s{roomGroups()};
        s.queue.on(setLight(kWallA, {}, 100));
        s.queue.on(setLight(kWallB, {}, 100));
        s.queue.on(setLight(kWallC, {}, 120));
        s.drain(); // A and B want the same, but no group is just the two
        expect(s.sent.size() == 3 && std::ranges::none_of(s.sent, [](const SendLight &x) { return x.to.group; }));
    };

    "past the broadcast budget, group commands go to each light"_test = [] {
        Sends s{roomGroups()};
        for (uint8_t i = 0; i < core::SendQueue::kMaxGroupSends + 1; ++i) {
            for (const Role role : {kWallB, kWallD}) {
                s.queue.on(setLight(role, {}, static_cast<uint8_t>(10 + i)));
            }
            s.drain();
        }
        expect(s.to(kAlertGroup).size() == core::SendQueue::kMaxGroupSends);
        expect(s.to(kWallB).size() == 1 && s.to(kWallD).size() == 1);

        s.sent.clear();
        s.now += core::SendQueue::kGroupSendWindow;
        s.queue.on(setLight(kWallB, {}, 1));
        s.queue.on(setLight(kWallD, {}, 1));
        s.drain(); // the window's passed
        expect(s.sent.size() == 1 && s.to(kAlertGroup).size() == 1);
    };
};
