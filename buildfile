cxx.std = latest
cxx.features.modules = true

using cxx

assert $cxx.features.modules "the configured C++ compiler must support modules"

cxx.coptions += -Wall -Wextra

if ($getenv('SUNSIM_WASM') == '1')
{
  cxx.coptions += -include $src_root/test/build2/emscripten_std_compat.hpp
  exe{sunsim_web_simulator}: cxx.loptions += -sMODULARIZE=1 -sEXPORT_ES6=1 \
      -sEXPORTED_RUNTIME_METHODS=UTF8ToString --no-entry
  exe{sunsim_web_simulator}: extension = js
}

mxx{*}: extension = cppm
cxx{*}: extension = cxx
# The source name does not match the primary module name.
mxx{components/sunsim_presentation/src/status_display}: cxx.module_name = sunsim.presentation

# Keep each portable executable's module interfaces in its target so the
# module mapper can discover the full import graph without installed packages.
exe{sunsim_build2_smoke}: cxx{test/build2/smoke.cxx} \
    mxx{components/sunsim_events/src/*.cppm} \
    mxx{components/sunsim_core/src/**.cppm} \
    mxx{components/sunsim_weather/src/*.cppm} \
    mxx{components/sunsim_presentation/src/*.cppm}

exe{sunsim_web_simulator}: cxx{web/simulator.cxx} \
    mxx{components/sunsim_events/src/*.cppm} \
    mxx{components/sunsim_core/src/**.cppm} \
    mxx{components/sunsim_weather/src/*.cppm} \
    mxx{components/sunsim_presentation/src/*.cppm}

alias{portable}: exe{sunsim_build2_smoke}
alias{wasm}: exe{sunsim_web_simulator}
