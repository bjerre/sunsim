#include "zigbee_shim.h"

ezb_zha_color_dimmer_switch_config_t sunsim_zha_color_dimmer_switch_config(void)
{
    return (ezb_zha_color_dimmer_switch_config_t)EZB_ZHA_COLOR_DIMMER_SWITCH_CONFIG();
}

ezb_zha_on_off_light_config_t sunsim_zha_on_off_light_config(void)
{
    return (ezb_zha_on_off_light_config_t)EZB_ZHA_ON_OFF_LIGHT_CONFIG();
}
