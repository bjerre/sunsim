// What every host test uses: the core, attached like the firmware's, with a
// log of everything posted; a fixed day to run it on; and the test runner.
// (CHECK, a macro, is in check.h.)
export module sunsim.test;

import std.compat;
import sunsim.ev;
import sunsim.core;

export namespace sunsim::testing {

using namespace sunsim::ev;
using namespace std::chrono_literals;

// Runs one test, printing PASS or FAIL with its name.
void test(const char *name, const std::function<void()> &body);
// A failed CHECK (see check.h).
void fail(const char *file, int line, const char *condition);
// How many CHECKs have failed so far.
[[nodiscard]] int failures() noexcept;

constexpr Role kWallA{Group::Wall, 0}; // the east end
constexpr Role kWallB{Group::Wall, 1};
constexpr Role kWallC{Group::Wall, 2};
constexpr Role kWallE{Group::Wall, 4}; // the west end
constexpr Role kWork{Group::Work};
constexpr Role kButton{Group::Button};


// noon()'s day: a Tuesday, the 172nd day of the year.
constexpr std::chrono::local_days kTuesday{std::chrono::year{2022} / std::chrono::June / 21};

// That Tuesday at `time` after midnight, clock set, five hours behind UTC.
TimeReading at(std::chrono::minutes time)
{
    return {.local = kTuesday + time, .utc_offset = -5h, .valid = true};
}

TimeReading noon()
{
    return at(12h);
}

// The core, wired like the firmware, plus a log of every event posted.
struct Harness {
    EventBus                 bus;
    core::CoreModules        core{bus.post(), {}};
    std::vector<Event>       log;
    Instant                  now{};

    Harness()
    {
        core::wireCore(bus, core);
        // Record everything posted, including platform-bound events.
        recordAll(std::make_index_sequence<std::variant_size_v<Payload>>{});
        post(NetworkUp{}); // at time 0, so "15 s after the network is up" is 15 s in
        clearLog();
    }

    template <std::size_t... I> void recordAll(std::index_sequence<I...>)
    {
        (bus.on<std::variant_alternative_t<I, Payload>>(
             [this](const std::variant_alternative_t<I, Payload> &e) { log.emplace_back(e); }),
         ...);
    }

    void post(Event e)
    {
        bus.post()(std::move(e));
        bus.drain();
    }

    void tick(int seconds = 1, TimeReading time = noon())
    {
        for (int i = 0; i < seconds; ++i) {
            now += 1s;
            post(Tick{.at = now, .time = time});
        }
    }

    template <class T> std::vector<T> all() const
    {
        std::vector<T> out;
        for (const auto &e : log) {
            if (const auto *p = std::get_if<T>(&e.data)) {
                out.push_back(*p);
            }
        }
        return out;
    }

    void clearLog() { log.clear(); }

    // Pair `role` through the console flow and complete discovery.
    void pairBulb(Role role, uint64_t ext, uint16_t short_addr, uint8_t ep = 1)
    {
        post(PairRequested{role});
        post(DeviceAnnounced{.ext_addr = ext, .short_addr = short_addr});
        post(EndpointFound{.role = role, .endpoint = ep});
    }
};

// A light's or a command's colour, if it's a C.
template <class C> const C *colorOf(const LightState &s)
{
    return std::get_if<C>(&s.color);
}
template <class C> const C *colorOf(const SetLight &s)
{
    return s.color ? std::get_if<C>(&*s.color) : nullptr;
}

// A forecast for noon()'s date: 20% at 12:00, 60% at 13:00, the next day's
// noon 90%.
inline CloudCover tuesdayForecast()
{
    CloudCover f;
    f.day         = kTuesday;
    f.percent[12] = 20;
    f.percent[13] = 60;
    f.percent[36] = 90;
    return f;
}

} // namespace sunsim::testing

namespace sunsim::testing {

namespace {
int g_failures = 0;
} // namespace

void test(const char *name, const std::function<void()> &body)
{
    const int before = g_failures;
    body();
    std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name);
}

void fail(const char *file, int line, const char *condition)
{
    std::printf("    FAIL %s:%d: %s\n", file, line, condition);
    ++g_failures;
}

int failures() noexcept
{
    return g_failures;
}

} // namespace sunsim::testing
