#include <cstdlib>

#include "esp_check.h"
#include "nvs_flash.h"

#include "sunsim_app.h"

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    setenv("TZ", CONFIG_SUNSIM_TZ, 1);
    tzset();

    sunsim::SunsimApp::instance().run();
}
