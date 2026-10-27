#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.core;
import sunsim.ev;
import sunsim.presentation;
import sunsim.weather;

int main()
{
    sunsim::core::CoreRuntime runtime{sunsim::ev::Addresses{}};
    const std::string clock = sunsim::presentation::clockText(std::chrono::hours{18});
    const auto flow = sunsim::presentation::makeProgramFlowDisplayData(sunsim::ev::StatusSnapshot{});
    return clock == "18:00" && !runtime.process() && flow.zone_count == sunsim::ev::kZoneCount &&
                   flow.zones[0].name == "office" && flow.zones[0].member_count == 6 &&
                   flow.zones[0].mode_count == 4 &&
                   flow.zones[0].available_modes[2].mode == sunsim::ev::Mode::RedAlert &&
                   flow.zones[0].available_modes[2].button_press == sunsim::ev::Press::Double &&
                   flow.zones[0].gate_applies &&
                   flow.zones[1].member_count == 1 && flow.zones[1].mode_count == 1 &&
                   flow.zones[1].available_modes[0].mode == sunsim::ev::Mode::Daylight &&
                   !flow.zones[1].available_modes[0].button_press &&
                   flow.zones[2].member_count == 1 && flow.zones[2].mode_count == 1
               ? 0
               : 1;
}
