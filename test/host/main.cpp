// Host tests: the real core modules, attached with the same core::wireCore()
// as the firmware, driven by events; a recorder captures everything posted.
// One file per area; each runs its tests. boost.ut prints the failures and
// the summary, and the exit status is 0 only if every expect() held.

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
}
