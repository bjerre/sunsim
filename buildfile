cxx.std = latest
cxx.features.modules = true

using cxx

assert $cxx.features.modules "the configured C++ compiler must support modules"

cxx.coptions += -Wall -Wextra

mxx{*}: extension = cppm
cxx{*}: extension = cxx
# The source name does not match the primary module name.
mxx{components/sunsim_presentation/src/status_display}: cxx.module_name = sunsim.presentation

# Keep the portable module interfaces in one build2 target so its module
# mapper can discover the full import graph without installed-module packages.
exe{sunsim_build2_smoke}: cxx{test/build2/smoke.cxx} \
    mxx{components/sunsim_events/src/*.cppm} \
    mxx{components/sunsim_core/src/**.cppm} \
    mxx{components/sunsim_weather/src/*.cppm} \
    mxx{components/sunsim_presentation/src/*.cppm}

alias{portable}: exe{sunsim_build2_smoke}
