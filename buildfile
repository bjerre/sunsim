cxx.std = latest
cxx.features.modules = true

using cxx

assert $cxx.features.modules "the configured C++ compiler must support modules"

cxx.coptions += -Wall -Wextra

if ($getenv('SUNSIM_WASM') == '1')
{
  cxx.coptions += -include $src_root/test/build2/emscripten_std_compat.hpp
  cxx.loptions += -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORTED_RUNTIME_METHODS=UTF8ToString
  exe{sunsim_build2_smoke}: extension = js
}

mxx{*}: extension = cppm
cxx{*}: extension = cxx
# The source name does not match the primary module name.
mxx{components/sunsim_presentation/src/status_display}: cxx.module_name = sunsim.presentation

# Keep the portable module interfaces in one build2 target so its module
# mapper can discover the full import graph without installed-module packages.
exe{sunsim_build2_smoke}: cxx{test/build2/smoke.cxx} cxx{web/simulator.cxx} \
    mxx{components/sunsim_events/src/*.cppm} \
    mxx{components/sunsim_core/src/**.cppm} \
    mxx{components/sunsim_weather/src/*.cppm} \
    mxx{components/sunsim_presentation/src/*.cppm}

alias{portable}: exe{sunsim_build2_smoke}
alias{wasm}: exe{sunsim_build2_smoke}
