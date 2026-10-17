// Text formatting shared by the console and the status display.
module sunsim.platform:format; // internal: not exported

import std.compat;
import sunsim.ev;

namespace sunsim::platform {

// A time after midnight as "HH:MM". 24:00 stays as is (a window ending at
// midnight); anything later wraps into the next day.
inline std::string clockText(ev::Hours after_midnight)
{
    auto t = std::chrono::round<std::chrono::minutes>(after_midnight);
    if (t > std::chrono::days{1}) {
        t -= std::chrono::days{1};
    }
    return std::format("{:02}:{:02}", std::chrono::floor<std::chrono::hours>(t).count(),
                       (t % std::chrono::hours{1}).count());
}

// A duration as "Xh MMm".
inline std::string durationText(ev::Hours d)
{
    const auto t = std::chrono::round<std::chrono::minutes>(d);
    return std::format("{}h {:02}m", std::chrono::floor<std::chrono::hours>(t).count(), (t % std::chrono::hours{1}).count());
}

} // namespace sunsim::platform
