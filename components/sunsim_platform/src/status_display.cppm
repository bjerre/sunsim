module;

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
#endif

export module sunsim.platform:status_display;

import std.compat;
import sunsim.ev;
import :status_slot;
import :format;

export namespace sunsim::platform {

// Drives the on-board ST7789 LCD (Waveshare ESP32-C6-GEEK by default, see
// the "Status display" menuconfig): current time, day length, countdown to
// the next sunrise/sunset, mode, the sky (an alert, else rain or snow, else
// cloud cover), and one dot per device for its link state,
// grouped: "D ●●●●●  W ●  B ●" (the daylight wall east to west, the work
// light, the button).
// Redraws once a second from the latest StatusSnapshot - the same data as
// the `status` console command. No-op unless CONFIG_SUNSIM_DISPLAY.
void startStatusDisplay(const StatusSlot &status);

} // namespace sunsim::platform

#if CONFIG_SUNSIM_DISPLAY

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
    lv_obj_t *cloud;
    ev::PerRole<lv_obj_t *> dots;
};

struct Context {
    const StatusSlot *status;
    Ui                ui;
};

// The device dots: one row, each group's letter followed by a dot per
// member in order (the wall's east to west).
constexpr int kDotSize   = 10;
constexpr int kDotPitch  = 16; // a dot and the gap after it
constexpr int kLabelCell = 16; // a group letter and the gap after it
constexpr int kGroupGap  = 10; // between one group's last dot and the next letter
constexpr int kDotsLeft  = 8;
constexpr int kDotsTopY  = 114;

constexpr int dotsRowWidth()
{
    int width = 0;
    for (const ev::GroupSpec &g : ev::kLayout) {
        width += kLabelCell + g.count * kDotPitch + kGroupGap;
    }
    return width - kGroupGap;
}
static_assert(kDotsLeft + dotsRowWidth() <= kWidth, "the device dots don't fit in one row (see ev::kLayout)");

// A group's letter on the display: D = the daylight wall, W = the work
// light, B = the button.
char groupLabel(ev::Group group)
{
    switch (group) {
    case ev::Group::Wall:
        return 'D';
    case ev::Group::Work:
        return 'W';
    case ev::Group::Button:
        return 'B';
    }
    return '?';
}

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
    ui.cloud      = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 58);

    int x = kDotsLeft;
    for (const ev::GroupSpec &g : ev::kLayout) {
        lv_obj_t *label = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, x, kDotsTopY);
        lv_obj_set_style_text_color(label, lv_color_hex(0xaaaaaa), 0);
        const char text[] = {groupLabel(g.group), '\0'};
        lv_label_set_text(label, text); // copies it
        x += kLabelCell;

        for (const ev::Role role : ev::members(g.group)) {
            lv_obj_t *dot = lv_obj_create(scr);
            lv_obj_set_size(dot, kDotSize, kDotSize);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_bg_color(dot, linkColor(ev::DeviceLink::Unpaired), 0);
            lv_obj_set_scrollable(dot, false);
            lv_obj_set_pos(dot, x, kDotsTopY + 4);
            ui.dots[role] = dot;
            x += kDotPitch;
        }
        x += kGroupGap;
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
    case ev::Mode::Tuning:
        lv_label_set_text(ui.mode, "tuning");
        lv_obj_set_style_text_color(ui.mode, lv_color_hex(0x7fd4ff), 0);
        break;
    }

    // The sky: the worst alert in its colour, else what's falling, else cloud.
    constexpr uint32_t kSkyColor = 0x9ab8d8; // grey-blue
    uint32_t           sky_color = kSkyColor;
    if (st.alerts.count > 0) {
        const ev::AlertLevel level = st.alerts.items[0].level;
        sky_color = level == ev::AlertLevel::Warning ? 0xff3030 : level == ev::AlertLevel::Watch ? 0xff9a30 : 0xffe040;
        lv_label_set_text(ui.cloud, ev::alertLevelName(level));
    } else if (st.conditions && st.conditions->precip != ev::Precip::None) {
        lv_label_set_text(ui.cloud, ev::precipName(st.conditions->precip));
    } else if (st.cloud_percent) {
        lv_label_set_text_fmt(ui.cloud, "cloud %u%%", *st.cloud_percent);
    } else {
        lv_label_set_text(ui.cloud, "");
    }
    lv_obj_set_style_text_color(ui.cloud, lv_color_hex(sky_color), 0);

    for (const ev::Role role : ev::kRoles) {
        lv_obj_set_style_bg_color(ui.dots[role], linkColor(st.devices[role].link), 0);
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
