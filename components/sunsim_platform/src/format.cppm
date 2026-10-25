// Text formatting shared by the console and the status display.
module sunsim.platform:format; // internal: not exported

import std.compat;
import sunsim.ev;
import sunsim.presentation;

namespace sunsim::platform {

// Keep the platform's existing formatting names while sharing the formatting
// rules with the platform-neutral status presentation model.
inline std::string clockText(ev::Hours after_midnight)
{
    return presentation::clockText(after_midnight);
}

inline std::string durationText(ev::Hours duration)
{
    return presentation::durationText(duration);
}

} // namespace sunsim::platform
