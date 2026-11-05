//! The status display on the board: the Waveshare ESP32-C6-GEEK's 1.14"
//! ST7789 (240x135, landscape), over SPI. What it shows is
//! platform::display's; this draws it into a framebuffer once a second and
//! sends the frame when it changed. A port of the C++ status_display.cppm's
//! panel half, with mipidsi for the panel and embedded-graphics for LVGL.

use std::time::Duration;

use embedded_graphics::prelude::*;
use embedded_graphics::primitives::Rectangle;
use esp_idf_svc::hal::delay::FreeRtos;
use esp_idf_svc::hal::gpio::{AnyIOPin, Gpio1, Gpio2, Gpio3, Gpio4, Gpio5, Gpio6, PinDriver};
use esp_idf_svc::hal::spi::{Dma, SPI2, SpiConfig, SpiDeviceDriver, SpiDriverConfig};
use esp_idf_svc::hal::units::FromValueType;
use mipidsi::interface::SpiInterface;
use mipidsi::models::ST7789;
use mipidsi::options::{ColorInversion, Orientation, Rotation};
use platform::StatusSlot;
use platform::display::{Frame, HEIGHT, Screen, WIDTH, draw_splash};

const TAG: &str = "DISPLAY";

const PIXEL_CLOCK_MHZ: u32 = 40;

/// The panel's pins, as the C++ firmware's Kconfig has them (the
/// ESP32-C6-GEEK's): SCLK 1, MOSI 2, DC 3, RST 4, CS 5, backlight 6.
pub struct Pins {
    pub spi: SPI2<'static>,
    pub sclk: Gpio1<'static>,
    pub mosi: Gpio2<'static>,
    pub dc: Gpio3<'static>,
    pub rst: Gpio4<'static>,
    pub cs: Gpio5<'static>,
    pub backlight: Gpio6<'static>,
}

/// Starts the display on its own task: the name until the first status,
/// then the status, redrawn once a second. `flip`: the board upside down.
/// If the panel won't start, says so and carries on without it.
pub fn start(pins: Pins, status: StatusSlot, flip: bool) {
    let started = std::thread::Builder::new().name("display".into()).stack_size(8 * 1024).spawn(move || {
        if let Err(e) = run(pins, status, flip) {
            log::error!(target: TAG, "LCD failed ({e}); continuing without the display");
        }
    });
    if started.is_err() {
        log::error!(target: TAG, "No display task; continuing without the display");
    }
}

// What failed, and how.
fn failed(what: &str, e: impl std::fmt::Debug) -> String {
    format!("{what}: {e:?}")
}

fn run(pins: Pins, status: StatusSlot, flip: bool) -> Result<(), String> {
    let mut backlight = PinDriver::output(pins.backlight).map_err(|e| failed("backlight", e))?;
    let _ = backlight.set_low();

    let spi = SpiDeviceDriver::new_single(
        pins.spi,
        pins.sclk,
        pins.mosi,
        Option::<AnyIOPin>::None,
        Some(pins.cs),
        &SpiDriverConfig::new().dma(Dma::Auto(WIDTH as usize * 20 * 2)),
        &SpiConfig::new().baudrate(PIXEL_CLOCK_MHZ.MHz().into()),
    )
    .map_err(|e| failed("spi", e))?;
    let dc = PinDriver::output(pins.dc).map_err(|e| failed("dc", e))?;
    let rst = PinDriver::output(pins.rst).map_err(|e| failed("rst", e))?;
    let mut buffer = [0u8; 512];
    let interface = SpiInterface::new(spi, dc, &mut buffer);
    // The controller's RAM is 240x320; this panel shows a 135x240 window of
    // it (portrait), turned to landscape. IPS panels need the inversion.
    let rotation = if flip { Rotation::Deg270 } else { Rotation::Deg90 };
    let mut panel = mipidsi::Builder::new(ST7789, interface)
        .reset_pin(rst)
        .display_size(HEIGHT as u16, WIDTH as u16)
        .display_offset(52, 40)
        .orientation(Orientation::new().rotate(rotation))
        .invert_colors(ColorInversion::Inverted)
        .init(&mut FreeRtos)
        .map_err(|e| failed("ST7789", e))?;

    let whole = Rectangle::new(Point::zero(), Size::new(WIDTH, HEIGHT));
    let mut frame = Frame::default();
    draw_splash(&mut frame);
    panel.fill_contiguous(&whole, frame.pixels.iter().copied()).map_err(|e| failed("draw", e))?;
    let _ = backlight.set_high();
    log::info!(target: TAG, "Status display started");

    let mut shown: Option<Screen> = None;
    loop {
        if let Some(st) = status.get() {
            let screen = Screen::of(&st);
            if shown.as_ref() != Some(&screen) {
                screen.draw(&mut frame);
                panel.fill_contiguous(&whole, frame.pixels.iter().copied()).map_err(|e| failed("draw", e))?;
                shown = Some(screen);
            }
        }
        std::thread::sleep(Duration::from_secs(1));
    }
}
