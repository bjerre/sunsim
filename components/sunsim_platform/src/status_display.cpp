#include "sunsim/platform/status_display.h"

#include "sdkconfig.h"

#if CONFIG_SUNSIM_DISPLAY

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "format.h"
#include "sunsim/platform/status_slot.h"

namespace sunsim::platform {

namespace {
constexpr const char *kTag = "DISPLAY";

// ST7789 controller RAM is 240x320; this 1.14" panel shows a 240x135
// window of it, offset differently depending on which way it's rotated.
constexpr int kWidth  = 240;
constexpr int kHeight = 135;

#if CONFIG_SUNSIM_DISPLAY_FLIP
constexpr bool kFlip = true;
#else
constexpr bool kFlip = false;
#endif

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int               kPixelClockHz = 40 * 1000 * 1000;

struct Ui {
    lv_obj_t *time;
    lv_obj_t *weekday;
    lv_obj_t *mode;
    lv_obj_t *day_length;
    lv_obj_t *next_event;
    lv_obj_t *dots[ev::kRoleCount];
};

struct Context {
    const StatusSlot *status;
    Ui                ui;
};

// Short labels under each device dot, in Role order.
constexpr const char *kDotLabels[ev::kRoleCount] = {"A", "B", "C", "W", "Btn"};

lv_color_t linkColor(ev::DeviceLink link)
{
    switch (link) {
    case ev::DeviceLink::Online:
        return lv_color_hex(0x3ddc84); // green
    case ev::DeviceLink::Silent:
        return lv_color_hex(0xf5a623); // amber
    case ev::DeviceLink::Unpaired:
        break;
    }
    return lv_color_hex(0x3a3a3a); // dim grey
}

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, lv_align_t align, int x, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, align, x, y);
    lv_label_set_text(label, "");
    return label;
}

void buildUi(Ui &ui)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);

    ui.time       = makeLabel(scr, &lv_font_montserrat_48, LV_ALIGN_TOP_LEFT, 6, 0);
    ui.weekday    = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_RIGHT, -6, 6);
    ui.mode       = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 32);
    ui.day_length = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 6, 54);
    ui.next_event = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 6, 80);

    constexpr int kDotSize  = 10;
    constexpr int kSpacing  = kWidth / ev::kRoleCount;
    constexpr int kDotsTopY = 114;
    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        const int x = 10 + static_cast<int>(i) * kSpacing;

        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_set_size(dot, kDotSize, kDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_bg_color(dot, linkColor(ev::DeviceLink::Unpaired), 0);
        lv_obj_set_scrollable(dot, false);
        lv_obj_set_pos(dot, x, kDotsTopY + 4);
        ui.dots[i] = dot;

        lv_obj_t *label = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, x + kDotSize + 4, kDotsTopY);
        lv_obj_set_style_text_color(label, lv_color_hex(0xaaaaaa), 0);
        lv_label_set_text(label, kDotLabels[i]);
    }
}

void render(Ui &ui, const ev::StatusSnapshot &st)
{
    char a[16];
    char b[16];

    if (st.time.valid) {
        lv_label_set_text_fmt(ui.time, "%02d:%02d", st.time.local.tm_hour, st.time.local.tm_min);
        lv_label_set_text_fmt(ui.weekday, "%s%s", st.time.simulated ? "sim " : "", kWeekdayNames[st.time.local.tm_wday % 7]);

        formatDuration(a, sizeof(a), st.day.day_length_hours);
        lv_label_set_text_fmt(ui.day_length, "Day %s", a);

        formatDuration(a, sizeof(a), st.day.hours_until_next);
        formatClock(b, sizeof(b), st.day.next_event_hour);
        lv_label_set_text_fmt(ui.next_event, "%s %s  %s", st.day.next_is_sunrise ? "Rise" : "Set", a, b);
    } else {
        lv_label_set_text(ui.time, "--:--");
        lv_label_set_text(ui.weekday, "");
        lv_label_set_text(ui.day_length, "Waiting for");
        lv_label_set_text(ui.next_event, "time sync...");
    }

    switch (st.mode) {
    case ev::Mode::Daylight:
        lv_label_set_text(ui.mode, st.in_active_window ? "daylight" : "idle");
        lv_obj_set_style_text_color(ui.mode, lv_color_hex(st.in_active_window ? 0xffd27f : 0x888888), 0);
        break;
    case ev::Mode::Off:
        lv_label_set_text(ui.mode, "off");
        lv_obj_set_style_text_color(ui.mode, lv_color_hex(0x888888), 0);
        break;
    case ev::Mode::RedAlert:
        lv_label_set_text(ui.mode, "RED ALERT");
        lv_obj_set_style_text_color(ui.mode, lv_color_hex(0xff3030), 0);
        break;
    }

    for (std::size_t i = 0; i < ev::kRoleCount; ++i) {
        lv_obj_set_style_bg_color(ui.dots[i], linkColor(st.devices[i].link), 0);
    }
}

esp_err_t initPanel(esp_lcd_panel_io_handle_t &io, esp_lcd_panel_handle_t &panel)
{
    const gpio_config_t bl_cfg{
        .pin_bit_mask = 1ULL << CONFIG_SUNSIM_DISPLAY_PIN_BL,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), kTag, "backlight gpio");
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_BL), 0);

    spi_bus_config_t bus{};
    bus.sclk_io_num     = CONFIG_SUNSIM_DISPLAY_PIN_SCLK;
    bus.mosi_io_num     = CONFIG_SUNSIM_DISPLAY_PIN_MOSI;
    bus.miso_io_num     = -1;
    bus.quadwp_io_num   = -1;
    bus.quadhd_io_num   = -1;
    bus.max_transfer_sz = kWidth * 20 * sizeof(uint16_t);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &bus, SPI_DMA_CH_AUTO), kTag, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg{};
    io_cfg.cs_gpio_num       = static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_CS);
    io_cfg.dc_gpio_num       = static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_DC);
    io_cfg.spi_mode          = 0;
    io_cfg.pclk_hz           = kPixelClockHz;
    io_cfg.trans_queue_depth = 10;
    io_cfg.lcd_cmd_bits      = 8;
    io_cfg.lcd_param_bits    = 8;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost), &io_cfg, &io), kTag,
        "panel io");

    esp_lcd_panel_dev_config_t panel_cfg{};
    panel_cfg.reset_gpio_num = static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_RST);
    panel_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel), kTag, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), kTag, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), kTag, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, true), kTag, "invert"); // IPS panels need inversion
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), kTag, "on");
    return ESP_OK;
}

void displayTask(void *arg)
{
    auto &ctx = *static_cast<Context *>(arg);
    for (;;) {
        if (const auto st = ctx.status->get(); st && lvgl_port_lock(0)) {
            render(ctx.ui, *st);
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
} // namespace

void startStatusDisplay(const StatusSlot &status)
{
    esp_lcd_panel_io_handle_t io    = nullptr;
    esp_lcd_panel_handle_t    panel = nullptr;
    if (initPanel(io, panel) != ESP_OK) {
        ESP_LOGE(kTag, "LCD init failed; continuing without the display");
        return;
    }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "LVGL init failed; continuing without the display");
        return;
    }

    lvgl_port_display_cfg_t disp_cfg{};
    disp_cfg.io_handle     = io;
    disp_cfg.panel_handle  = panel;
    disp_cfg.buffer_size   = kWidth * 20;
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = kWidth;
    disp_cfg.vres          = kHeight;
    disp_cfg.rotation.swap_xy  = true;
    disp_cfg.rotation.mirror_x = !kFlip;
    disp_cfg.rotation.mirror_y = kFlip;
    disp_cfg.color_format     = LV_COLOR_FORMAT_RGB565;
    disp_cfg.flags.buff_dma   = 1;
    disp_cfg.flags.swap_bytes = 1;
    if (!lvgl_port_add_disp(&disp_cfg)) {
        ESP_LOGE(kTag, "LVGL display registration failed; continuing without the display");
        return;
    }

    // Landscape: the visible 240x135 window sits 40 columns / 52-53 rows
    // into the controller's RAM (row offset depends on the mirror).
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, !kFlip, kFlip);
    esp_lcd_panel_set_gap(panel, 40, kFlip ? 52 : 53);

    static Context ctx{.status = &status, .ui = {}};
    if (lvgl_port_lock(0)) {
        buildUi(ctx.ui);
        lv_label_set_text(ctx.ui.time, "sunsim");
        lvgl_port_unlock();
    }
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_BL), 1);

    xTaskCreate(&displayTask, "display", 4096, &ctx, 2, nullptr);
    ESP_LOGI(kTag, "Status display started");
}

} // namespace sunsim::platform

#else

namespace sunsim::platform {
void startStatusDisplay(const StatusSlot &) {}
} // namespace sunsim::platform

#endif
