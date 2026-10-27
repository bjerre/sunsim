module;

#include "sdkconfig.h"

#if CONFIG_SUNSIM_DISPLAY
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

export module sunsim.platform:status_display;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import sunsim.ev;
import sunsim.presentation;
import :status_slot;

export namespace sunsim::platform {

// Drives the on-board ST7789 LCD (Waveshare ESP32-C6-GEEK by default, see
// the "Status display" menuconfig): current time, day length, countdown to
// the next sunrise/sunset, mode, the sky (an alert, else rain or snow, else
// cloud cover), and one dot per device for its link state,
// grouped: "D ●●●●● W ● L ● G ● B ●" (the daylight wall east to west, the
// torchiere, desk downlight, grow light, and button).
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
    lv_obj_t *sky;
    ev::PerRole<lv_obj_t *> dots;
};

struct Context {
    const StatusSlot *status;
    Ui                ui;
};

// The device dots: one row, each group's letter followed by a dot per
// member in order (the wall's east to west).
constexpr int kDotSize   = 10;
constexpr int kDotPitch  = 12; // a dot and a compact 2 px gap
constexpr int kLabelCell = 16; // a group letter and the gap after it
constexpr int kGroupGap  = 4;  // between one group's last dot and the next letter
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
    return lv_color_hex(0x3a3a3a); // dim gray
}

lv_color_t toneColor(presentation::Tone tone)
{
    switch (tone) {
    case presentation::Tone::Muted:
        return lv_color_hex(0x888888);
    case presentation::Tone::Daylight:
        return lv_color_hex(0xffd27f);
    case presentation::Tone::Tuning:
        return lv_color_hex(0x7fd4ff);
    case presentation::Tone::Advisory:
        return lv_color_hex(0xffe040);
    case presentation::Tone::Watch:
        return lv_color_hex(0xff9a30);
    case presentation::Tone::Warning:
        return lv_color_hex(0xff3030);
    case presentation::Tone::Sky:
        return lv_color_hex(0x9ab8d8);
    }
    return lv_color_white();
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

void buildUi(Ui &ui, const auto &groups)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);

    ui.time       = makeLabel(scr, &lv_font_montserrat_48, LV_ALIGN_TOP_LEFT, 6, 0);
    ui.weekday    = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_RIGHT, -6, 6);
    ui.mode       = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 32);
    ui.day_length = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 6, 54);
    ui.next_event = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_LEFT, 6, 80);
    ui.sky        = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 58);

    int x = kDotsLeft;
    for (const presentation::DeviceGroupView &g : groups) {
        lv_obj_t *label = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, x, kDotsTopY);
        lv_obj_set_style_text_color(label, lv_color_hex(0xaaaaaa), 0);
        const char text[] = {g.label, '\0'};
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

void render(Ui &ui, const presentation::StatusDisplayData &data)
{
    lv_label_set_text(ui.time, data.time.c_str());
    lv_label_set_text(ui.weekday, data.weekday.c_str());
    lv_label_set_text(ui.day_length, data.day_length.c_str());
    lv_label_set_text(ui.next_event, data.next_event.c_str());
    lv_label_set_text(ui.mode, data.mode.c_str());
    lv_obj_set_style_text_color(ui.mode, toneColor(data.mode_tone), 0);
    lv_label_set_text(ui.sky, data.sky.c_str());
    lv_obj_set_style_text_color(ui.sky, toneColor(data.sky_tone), 0);

    for (const ev::Role role : ev::kRoles) {
        lv_obj_set_style_bg_color(ui.dots[role], linkColor(data.device_links[role]), 0);
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
        if (const auto st = ctx.status->get()) {
            const presentation::StatusDisplayData data = presentation::makeStatusDisplayData(*st);
            if (lvgl_port_lock(0)) {
                render(ctx.ui, data);
                lvgl_port_unlock();
            }
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
        ev::log::error(kTag, "LCD init failed; continuing without the display");
        return;
    }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) {
        ev::log::error(kTag, "LVGL init failed; continuing without the display");
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
        ev::log::error(kTag, "LVGL display registration failed; continuing without the display");
        return;
    }

    // Landscape: the visible 240x135 window sits 40 columns / 52-53 rows
    // into the controller's RAM (row offset depends on the mirror).
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, !kFlip, kFlip);
    esp_lcd_panel_set_gap(panel, 40, kFlip ? 52 : 53);

    static Context ctx{.status = &status, .ui = {}};
    if (lvgl_port_lock(0)) {
        buildUi(ctx.ui, presentation::kDeviceGroups);
        lv_label_set_text(ctx.ui.time, "sunsim");
        lvgl_port_unlock();
    }
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_SUNSIM_DISPLAY_PIN_BL), 1);

    xTaskCreate(&displayTask, "display", 4096, &ctx, 2, nullptr);
    ev::log::info(kTag, "Status display started");
}

} // namespace sunsim::platform

#else

namespace sunsim::platform {
void startStatusDisplay(const StatusSlot &) {}
} // namespace sunsim::platform

#endif
