#include <cstdlib>

#include "esp_check.h"
#include "nvs_flash.h"

#include "sunsim_app.h"
#include "touchlink_reset_tool.h"

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    setenv("TZ", CONFIG_SUNSIM_TZ, 1);
    tzset();

#if CONFIG_SUNSIM_TOUCHLINK_RESET_TOOL
    sunsim::runTouchlinkResetTool();
#else
    sunsim::SunsimApp::instance().run();
#endif
}
