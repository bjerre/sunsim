#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.core;
import sunsim.ev;
import sunsim.presentation;
import sunsim.weather;

namespace {

bool profileParsingWorks()
{
    constexpr std::string_view profile = R"json({
      "zones": [{
        "members": [
          {"look":"sun-curve","role":"WallA"},
          {"role":"WallB","look":"sun-curve"},
          {"role":"WallC","look":"sun-curve"},
          {"role":"WallD","look":"sun-curve"},
          {"role":"WallE","look":"sun-curve"},
          {"role":"Work","look":"work-curve","peakPercent":90},
          {"role":"Desk","look":"on-off"},
          {"role":"Grow","look":"on-off"}
        ],
        "weather":{"alerts":["WallD"],"precipitation":["WallC"]},
        "buttonRole":"Button",
        "endMinute":1140,
        "startMinute":480,
        "name":"room \u2603"
      }],
      "schemaVersion":1
    })json";
    sunsim::ev::InstallationProfileBuilder builder;
    if (!builder.parseJson(profile)) {
        return false;
    }
    const auto &installation = builder.installation();
    const auto &zone = installation.zones[0];
    sunsim::ev::InstallationProfileBuilder invalid;
    return builder.ready() && installation.zone_count == 1 &&
           zone.name == "room \xe2\x98\x83" && zone.lights.size() == 8 &&
           zone.controls.size() == 1 && zone.weather.has_value() &&
           !invalid.parseJson(R"({"schemaVersion":1,"zones":[]} trailing)") &&
           !invalid.parseJson(R"({"schemaVersion":1,"schemaVersion":1,"zones":[]})");
}

} // namespace

int main()
{
    sunsim::core::CoreRuntime runtime{sunsim::ev::Addresses{}};
    auto alternate_zones = sunsim::ev::kZones;
    alternate_zones[0].name = "main-room";
    alternate_zones[1].gate = {sunsim::ev::Window{
        .hours = sunsim::ev::DaySpan{std::chrono::hours{9}, std::chrono::hours{17}}}};
    const sunsim::ev::Installation alternate = sunsim::ev::installationOf(alternate_zones);
    sunsim::core::CoreRuntime alternate_runtime{sunsim::ev::Addresses{}, alternate};
    sunsim::ev::StatusSnapshot alternate_status;
    const auto alternate_flow =
        sunsim::presentation::makeProgramFlowDisplayData(alternate_status, alternate);
    const sunsim::ev::TimeReading at_eight_thirty{
        .local = std::chrono::local_seconds{
            std::chrono::local_days{std::chrono::year{2025} / std::chrono::January / 15} +
            std::chrono::hours{8} + std::chrono::minutes{30}},
        .valid = true};
    const std::string clock = sunsim::presentation::clockText(std::chrono::hours{18});
    const auto flow = sunsim::presentation::makeProgramFlowDisplayData(sunsim::ev::StatusSnapshot{});
    return profileParsingWorks() && clock == "18:00" && !runtime.process() &&
                   flow.zone_count == sunsim::ev::kZoneCount &&
                   flow.zones[0].name == "office" && flow.zones[0].member_count == 6 &&
                   flow.zones[0].mode_count == 4 &&
                   flow.zones[0].available_modes[2].mode == sunsim::ev::Mode::RedAlert &&
                   flow.zones[0].available_modes[2].button_press == sunsim::ev::Press::Double &&
                   flow.zones[0].gate_applies &&
                   flow.zones[1].member_count == 1 && flow.zones[1].mode_count == 1 &&
                   flow.zones[1].available_modes[0].mode == sunsim::ev::Mode::Daylight &&
                   !flow.zones[1].available_modes[0].button_press &&
                   flow.zones[2].member_count == 1 && flow.zones[2].mode_count == 1
                   && !alternate_runtime.process() && alternate_flow.zone_count == 3 &&
                   alternate_flow.zones[0].name == "main-room" &&
                   !sunsim::core::openness(alternate.spec({1}).gate, at_eight_thirty, sunsim::ev::ProgramConfig{})
               ? 0
               : 1;
}
