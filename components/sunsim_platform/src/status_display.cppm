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
    std::array<lv_obj_t *, 5> sky_bands{};
    std::array<lv_obj_t *, 13> sun_path{};
    std::array<lv_obj_t *, 6> skyline{};
    std::array<lv_obj_t *, 4> cloud{};
    std::array<lv_obj_t *, 3> precipitation{};
    lv_obj_t *horizon;
    lv_obj_t *sun_glow;
    lv_obj_t *sun_disc;
    lv_obj_t *event_backdrop;
    lv_obj_t *link_backdrop;
    lv_obj_t *time;
    lv_obj_t *weekday;
    lv_obj_t *mode;
    lv_obj_t *day_length;
    lv_obj_t *next_event;
    lv_obj_t *sunrise;
    lv_obj_t *sunset;
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
constexpr int kDotsTopY  = 118;

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

lv_obj_t *makeShape(lv_obj_t *parent, int x, int y, int width, int height, uint32_t color,
                    int radius = 0)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_set_size(shape, width, height);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_style_bg_color(shape, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shape, 0, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_set_scrollable(shape, false);
    return shape;
}

void setVisible(lv_obj_t *obj, bool visible)
{
    lv_obj_set_hidden(obj, !visible);
}

void buildUi(Ui &ui, const auto &groups)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);

    for (std::size_t i = 0; i < ui.sky_bands.size(); ++i) {
        ui.sky_bands[i] = makeShape(scr, 0, static_cast<int>(i) * 21, kWidth, 22, 0x10243b);
    }

    for (std::size_t i = 0; i < ui.sun_path.size(); ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(ui.sun_path.size() - 1);
        const int x = 18 + static_cast<int>(t * 204.0);
        const int y = 102 - static_cast<int>(std::sin(std::numbers::pi * t) * 29.0);
        ui.sun_path[i] = makeShape(scr, x - 2, y - 2, 4, 4, 0xffdba0, LV_RADIUS_CIRCLE);
        lv_obj_set_style_bg_opa(ui.sun_path[i], static_cast<lv_opa_t>(145), 0);
    }

    // Soft horizon silhouette: a low office skyline below the sun's path.
    constexpr std::array<std::array<int, 3>, 6> buildings{{
        {{8, 24, 16}}, {{39, 19, 11}}, {{66, 31, 22}},
        {{105, 23, 14}}, {{137, 28, 19}}, {{174, 48, 13}},
    }};
    for (std::size_t i = 0; i < buildings.size(); ++i) {
        const auto &[x, width, height] = buildings[i];
        ui.skyline[i] = makeShape(scr, x, 133 - height, width, height, 0x172a43);
    }
    ui.horizon = makeShape(scr, 0, 103, kWidth, 2, 0xffd09a);
    lv_obj_set_style_bg_opa(ui.horizon, static_cast<lv_opa_t>(170), 0);

    // One compact cloud, with rain/snow marks when current conditions say
    // precipitation is falling.
    ui.cloud[0] = makeShape(scr, 142, 75, 35, 8, 0xc7d4de, LV_RADIUS_CIRCLE);
    ui.cloud[1] = makeShape(scr, 147, 67, 16, 16, 0xc7d4de, LV_RADIUS_CIRCLE);
    ui.cloud[2] = makeShape(scr, 158, 63, 20, 20, 0xc7d4de, LV_RADIUS_CIRCLE);
    ui.cloud[3] = makeShape(scr, 170, 69, 14, 14, 0xc7d4de, LV_RADIUS_CIRCLE);
    for (lv_obj_t *part : ui.cloud) {
        setVisible(part, false);
    }
    for (std::size_t i = 0; i < ui.precipitation.size(); ++i) {
        const uint32_t color = 0xa8e5ff;
        ui.precipitation[i] = makeShape(scr, 151 + static_cast<int>(i) * 10, 83, 3, 7,
                                        color, LV_RADIUS_CIRCLE);
        setVisible(ui.precipitation[i], false);
    }

    ui.sun_glow = makeShape(scr, 0, 0, 25, 25, 0xffbd69, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(ui.sun_glow, static_cast<lv_opa_t>(70), 0);
    ui.sun_disc = makeShape(scr, 0, 0, 13, 13, 0xffdf91, LV_RADIUS_CIRCLE);

    ui.event_backdrop = makeShape(scr, 46, 103, 148, 15, 0x0c1929, 7);
    lv_obj_set_style_bg_opa(ui.event_backdrop, static_cast<lv_opa_t>(160), 0);
    ui.link_backdrop = makeShape(scr, 0, 118, kWidth, 17, 0x081321);

    ui.time       = makeLabel(scr, &lv_font_montserrat_48, LV_ALIGN_TOP_LEFT, 6, 0);
    ui.weekday    = makeLabel(scr, &lv_font_montserrat_20, LV_ALIGN_TOP_RIGHT, -6, 4);
    ui.mode       = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 29);
    ui.sky        = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 47);
    ui.day_length = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 7, 54);
    ui.sunrise    = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, 6, 87);
    ui.sunset     = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_RIGHT, -6, 87);
    ui.next_event = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_MID, 0, 102);

    int x = kDotsLeft;
    for (const presentation::DeviceGroupView &g : groups) {
        lv_obj_t *label = makeLabel(scr, &lv_font_montserrat_14, LV_ALIGN_TOP_LEFT, x, kDotsTopY);
        lv_obj_set_style_text_color(label, lv_color_hex(0xaaaaaa), 0);
        const char text[] = {g.label, '\0'};
        lv_label_set_text(label, text); // copies it
        x += kLabelCell;

        for (const ev::Role role : ev::members(g.group)) {
            lv_obj_t *dot = makeShape(scr, x, kDotsTopY + 4, kDotSize, kDotSize, 0x3a3a3a, LV_RADIUS_CIRCLE);
            ui.dots[role] = dot;
            x += kDotPitch;
        }
        x += kGroupGap;
    }
}

void render(Ui &ui, const presentation::StatusDisplayData &data)
{
    constexpr std::array<std::array<uint32_t, 5>, 4> palettes{{
        {{0x050d22, 0x091733, 0x112449, 0x182c50, 0x1d3152}}, // night
        {{0x182044, 0x343b68, 0x765778, 0xd8836c, 0xf5a36d}}, // twilight
        {{0x425b91, 0x7b71a6, 0xc98289, 0xf3a274, 0xffd08a}}, // golden hour
        {{0x2777ae, 0x3489bd, 0x55a5ce, 0x7bbbd2, 0xa6d0d3}}, // day
    }};
    const std::size_t palette = !data.time_valid || data.sun_elevation_deg < -6.0 ? 0
                                : data.sun_elevation_deg < 0.0                ? 1
                                : data.sun_elevation_deg < 12.0               ? 2
                                                                               : 3;
    for (std::size_t i = 0; i < ui.sky_bands.size(); ++i) {
        lv_obj_set_style_bg_color(ui.sky_bands[i], lv_color_hex(palettes[palette][i]), 0);
    }

    lv_label_set_text(ui.time, data.time.c_str());
    lv_label_set_text(ui.weekday, data.weekday.c_str());
    lv_label_set_text(ui.day_length, data.day_length.c_str());
    lv_label_set_text(ui.next_event, data.next_event.c_str());
    lv_label_set_text(ui.sunrise, data.sunrise.c_str());
    lv_label_set_text(ui.sunset, data.sunset.c_str());
    lv_label_set_text(ui.mode, data.mode.c_str());
    lv_obj_set_style_text_color(ui.mode, toneColor(data.mode_tone), 0);
    lv_label_set_text(ui.sky, data.sky.c_str());
    lv_obj_set_style_text_color(ui.sky, toneColor(data.sky_tone), 0);

    setVisible(ui.sun_glow, data.time_valid && data.sun_elevation_deg > -1.0);
    setVisible(ui.sun_disc, data.time_valid);
    if (data.time_valid) {
        constexpr double kDegToRad = std::numbers::pi / 180.0;
        const double azimuth = data.sun_azimuth_deg * kDegToRad;
        const int sun_x = 120 - static_cast<int>(std::sin(azimuth) * 101.0);
        const int sun_y = std::clamp(102 - static_cast<int>(data.sun_elevation_deg * 0.40), 68, 111);
        lv_obj_set_pos(ui.sun_glow, sun_x - 12, sun_y - 12);
        lv_obj_set_pos(ui.sun_disc, sun_x - 6, sun_y - 6);
        lv_obj_set_style_bg_color(ui.sun_disc,
                                  lv_color_hex(data.sun_elevation_deg < -1.0 ? 0xb2a18d : 0xffdf91), 0);
    }

    const bool show_cloud = data.precipitation != ev::Precip::None ||
                            (data.cloud_percent && *data.cloud_percent >= 25);
    for (lv_obj_t *part : ui.cloud) {
        setVisible(part, show_cloud);
    }
    const bool show_precipitation = show_cloud && data.precipitation != ev::Precip::None;
    const uint32_t precip_color = data.precipitation == ev::Precip::Snow ? 0xf1f6ff : 0xa8e5ff;
    for (std::size_t i = 0; i < ui.precipitation.size(); ++i) {
        setVisible(ui.precipitation[i], show_precipitation);
        lv_obj_set_style_bg_color(ui.precipitation[i], lv_color_hex(precip_color), 0);
    }
    if (show_cloud) {
        const uint8_t cloud_percent = data.cloud_percent.value_or(100);
        const lv_opa_t cloud_opacity = static_cast<lv_opa_t>(100 + (cloud_percent * 155U) / 100U);
        for (lv_obj_t *part : ui.cloud) {
            lv_obj_set_style_bg_opa(part, cloud_opacity, 0);
        }
    }

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
