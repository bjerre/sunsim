// Host tests: the real core modules, attached with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.
// One file per area; each runs its tests.

import std.compat;
import sunsim.test;

void devicesTests();
void modesTests();
void daylightTests();
void weatherTests();
void eventsTests();

int main()
{
    devicesTests();
    modesTests();
    daylightTests();
    weatherTests();
    eventsTests();

    const int failed = sunsim::testing::failures();
    std::printf("\n%s (%d failure%s)\n", failed ? "FAILED" : "ALL PASSED", failed, failed == 1 ? "" : "s");
    return failed ? 1 : 0;
}
