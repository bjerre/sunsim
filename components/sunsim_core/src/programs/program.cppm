export module sunsim.core:program;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;

export namespace sunsim::core {

// A lighting program: decides what every light in its zone should show and
// posts it as LightTargets. It never deals with addresses, reachability or what was
// last sent; LightOutput does that, the same way for every program.
//
// ProgramHost owns one of each per zone and runs the one the zone's Mode
// selects. A program derives from Program (for posting, and its zone) and has:
//
//   static constexpr ev::Mode kMode           the mode it is
//   void refresh(role, now)                   re-apply `role` (nullopt = every light) now, with
//                                             a quick transition: a light (re)joined or answers
//                                             again, the network came up, a setting changed
//   void start(now)                           optional: it became the running program
//                                             (otherwise: refresh every light)
//   void on(const T &)                        optional, any events: Tick and ProgramTick reach
//                                             it only while it runs; settings, weather and the
//                                             console's tuning always, so it's current whenever
//                                             it starts
//
// `now` is nullopt until the clock's first Tick.
class Program {
public:
    // What every program is made with.
    struct Context {
        ev::Post   post;
        ev::ZoneId zone;
    };
    explicit Program(Context context) : post_(std::move(context.post)), zone_(context.zone) {}

    Program(const Program &)            = delete;
    Program &operator=(const Program &) = delete;

protected:
    ~Program();

    [[nodiscard]] ev::ZoneId zone() const noexcept { return zone_; }

    // Calls f(role) for each light in the zone, or only for `only` if it's
    // given (and one of them).
    template <class F> void forEachLight(std::optional<ev::Role> only, F &&f) const
    {
        for (const ev::Role role : ev::lights()) {
            if (ev::kZoneOf[role] == zone_ && (!only || *only == role)) {
                f(role);
            }
        }
    }

    void target(ev::Role role, const ev::LightState &state, ev::Transition level, ev::Transition color) const
    {
        post_(ev::LightTarget{{.role = role, .state = state, .level_transition = level, .color_transition = color}});
    }
    void target(ev::Role role, const ev::LightState &state, ev::Transition transition = {}) const
    {
        target(role, state, transition, transition);
    }

private:
    ev::Post   post_;
    ev::ZoneId zone_;
};

// What ProgramHost needs of a program.
template <class P>
concept LightingProgram = std::derived_from<P, Program> && std::same_as<decltype(P::kMode), const ev::Mode> &&
                          requires(P &p, std::optional<ev::Role> role, const std::optional<ev::TimeReading> &now) {
                              p.refresh(role, now);
                          };

// Not inline: sunsim.core's object file has the one copy. (Inline, GCC 15
// left the app, which destroys ProgramHost's tuple, linking to a destructor
// nobody emitted - as with Event's members and Timer's destructor.)
Program::~Program() = default;

} // namespace sunsim::core
