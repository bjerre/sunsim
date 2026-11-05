// What the Rust bindings are generated from: esp-zigbee-lib's header, and
// its function-like macros as functions (bindgen skips macros).
#pragma once

#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/zha.h"

ezb_zha_color_dimmer_switch_config_t sunsim_zha_color_dimmer_switch_config(void);
ezb_zha_on_off_light_config_t        sunsim_zha_on_off_light_config(void);
