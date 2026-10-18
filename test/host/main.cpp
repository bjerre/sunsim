// Host tests: the real core modules, attached with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.
// One file per area; each runs its tests. boost.ut prints the failures and
// the summary, and the exit status is 0 only if every expect() held.
//
//   sunsim_tests            every test
//   sunsim_tests "<name>"   the test so named (`*` and `?` match anything)
//   sunsim_tests --list     each test's name, a line apiece (how CTest finds
//                           them: discover_tests.cmake)

import std.compat;
import boost.ut;

void devicesTests();
void modesTests();
void daylightTests();
void weatherTests();
void eventsTests();

int main(int argc, const char **argv)
{
    const std::string_view arg  = argc > 1 ? argv[1] : "";
    const bool             list = arg == "--list";
    // A dry run still runs each body, with expect() a no-op, printing the names.
    boost::ut::cfg<boost::ut::override> = {.filter = list ? "" : arg, .dry_run = list};

    devicesTests();
    modesTests();
    daylightTests();
    weatherTests();
    eventsTests();
}
