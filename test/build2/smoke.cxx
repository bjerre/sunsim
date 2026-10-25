import std.compat;
import sunsim.core;
import sunsim.ev;
import sunsim.presentation;
import sunsim.weather;

int main()
{
    sunsim::core::CoreRuntime runtime{sunsim::ev::Addresses{}};
    const std::string clock = sunsim::presentation::clockText(std::chrono::hours{18});
    return clock == "18:00" && !runtime.process() ? 0 : 1;
}
