#include <cstdlib>
#include <ctime>

#include "esp_check.h"
#include "nvs_flash.h"

#include "app.h"

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    setenv("TZ", CONFIG_SUNSIM_TZ, 1);
    tzset();

    static sunsim::App app;
    app.run();
}
