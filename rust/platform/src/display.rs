//! The status display's picture: what the 240x135 LCD shows (the time, the
//! day, the next sunrise or sunset, the mode, the sky, and a dot per
//! device), from the latest StatusSnapshot - presentation's StatusView, in
//! the LCD's colors. Drawn with embedded-graphics onto whatever target it's given:
//! the firmware's framebuffer for the panel, a Frame in the tests. A port
//! of the C++ platform's status_display.cppm, minus the panel.

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{Circle, PrimitiveStyle};
use ev::*;
use u8g2_fonts::types::{FontColor, HorizontalAlignment, VerticalPosition};
use u8g2_fonts::{FontRenderer, fonts};

use crate::presentation::{StatusView, Tone, group_label};

/// The panel: a 1.14" ST7789's 240x135 window, landscape.
pub const WIDTH: u32 = 240;
pub const HEIGHT: u32 = 135;

// The device dots: one row, each group's letter followed by a dot per
// member in order (the wall's east to west).
const DOT_SIZE: u32 = 10;
const DOT_PITCH: i32 = 12; // a dot and a compact 2 px gap
const LABEL_CELL: i32 = 16; // a group letter and the gap after it
const GROUP_GAP: i32 = 4; // between one group's last dot and the next letter
const DOTS_LEFT: i32 = 8;
const DOTS_TOP_Y: i32 = 114;

const _: () = {
    let mut width = 0;
    let mut g = 0;
    while g < LAYOUT.len() {
        width += LABEL_CELL + LAYOUT[g].count as i32 * DOT_PITCH + GROUP_GAP;
        g += 1;
    }
    assert!(DOTS_LEFT + width - GROUP_GAP <= WIDTH as i32, "the device dots don't fit in one row (see LAYOUT)");
};

const fn rgb(hex: u32) -> Rgb565 {
    Rgb565::new(((hex >> 19) & 0x1F) as u8, ((hex >> 10) & 0x3F) as u8, ((hex >> 3) & 0x1F) as u8)
}

pub const BLACK: Rgb565 = rgb(0x000000);
pub const WHITE: Rgb565 = rgb(0xFFFFFF);
pub const LABEL_GRAY: Rgb565 = rgb(0xAAAAAA);
pub const ONLINE: Rgb565 = rgb(0x3DDC84); // green
pub const SILENT: Rgb565 = rgb(0xF5A623); // amber
pub const UNPAIRED: Rgb565 = rgb(0x3A3A3A); // dim gray
const DAYLIGHT: Rgb565 = rgb(0xFFD27F);
const IDLE: Rgb565 = rgb(0x888888);
pub const RED: Rgb565 = rgb(0xFF3030);
const TUNING: Rgb565 = rgb(0x7FD4FF);
const SKY: Rgb565 = rgb(0x9AB8D8); // gray-blue
const WATCH: Rgb565 = rgb(0xFF9A30);
const ADVISORY: Rgb565 = rgb(0xFFE040);

pub fn link_color(link: DeviceLink) -> Rgb565 {
    match link {
        DeviceLink::Online => ONLINE,
        DeviceLink::Silent => SILENT,
        DeviceLink::Unpaired => UNPAIRED,
    }
}

/// What the display shows, as text and colors.
#[derive(Clone, PartialEq, Debug)]
pub struct Screen {
    pub time: String,
    pub weekday: String,
    pub mode: (&'static str, Rgb565),
    pub day_length: String,
    pub next_event: String,
    pub sky: (String, Rgb565),
    pub dots: PerRole<Rgb565>,
}

/// A tone in the LCD's colors.
pub const fn tone_color(tone: Tone) -> Rgb565 {
    match tone {
        Tone::Muted => IDLE,
        Tone::Daylight => DAYLIGHT,
        Tone::Tuning => TUNING,
        Tone::Advisory => ADVISORY,
        Tone::Watch => WATCH,
        Tone::Warning => RED,
        Tone::Sky => SKY,
    }
}

impl Screen {
    pub fn of(st: &StatusSnapshot) -> Screen {
        let v = StatusView::of(st);
        let mut dots = PerRole::filled(UNPAIRED);
        for role in ROLES {
            dots[role] = link_color(v.links[role]);
        }
        Screen {
            time: v.time,
            weekday: v.weekday,
            mode: (v.mode, tone_color(v.mode_tone)),
            day_length: v.day_length,
            next_event: v.next_event,
            sky: (v.sky, tone_color(v.sky_tone)),
            dots,
        }
    }

    /// Draws it all, from a black background.
    pub fn draw<D: DrawTarget<Color = Rgb565>>(&self, target: &mut D) -> Result<(), D::Error> {
        target.clear(BLACK)?;
        let right = WIDTH as i32 - 6;
        text(TextSize::Time, &self.time, Point::new(6, 0), HorizontalAlignment::Left, WHITE, target)?;
        text(TextSize::Medium, &self.weekday, Point::new(right, 6), HorizontalAlignment::Right, WHITE, target)?;
        text(TextSize::Small, self.mode.0, Point::new(right, 32), HorizontalAlignment::Right, self.mode.1, target)?;
        text(TextSize::Medium, &self.day_length, Point::new(6, 54), HorizontalAlignment::Left, WHITE, target)?;
        text(TextSize::Medium, &self.next_event, Point::new(6, 80), HorizontalAlignment::Left, WHITE, target)?;
        text(TextSize::Small, &self.sky.0, Point::new(right, 58), HorizontalAlignment::Right, self.sky.1, target)?;

        let mut x = DOTS_LEFT;
        for g in LAYOUT {
            text(
                TextSize::Small,
                group_label(g.group),
                Point::new(x, DOTS_TOP_Y),
                HorizontalAlignment::Left,
                LABEL_GRAY,
                target,
            )?;
            x += LABEL_CELL;
            for &role in members(g.group) {
                dot(Point::new(x, DOTS_TOP_Y + 4), self.dots[role], target)?;
                x += DOT_PITCH;
            }
            x += GROUP_GAP;
        }
        Ok(())
    }
}

/// Where a device's dot is (its top-left), for the tests.
pub fn dot_at(role: Role) -> Point {
    let mut x = DOTS_LEFT;
    for g in LAYOUT {
        x += LABEL_CELL;
        for &r in members(g.group) {
            if r == role {
                return Point::new(x, DOTS_TOP_Y + 4);
            }
            x += DOT_PITCH;
        }
        x += GROUP_GAP;
    }
    unreachable!("every role is in a group")
}

/// What it shows before the first status: the name.
pub fn draw_splash<D: DrawTarget<Color = Rgb565>>(target: &mut D) -> Result<(), D::Error> {
    target.clear(BLACK)?;
    text(TextSize::Large, "sunsim", Point::new(6, 0), HorizontalAlignment::Left, WHITE, target)
}

fn dot<D: DrawTarget<Color = Rgb565>>(top_left: Point, color: Rgb565, target: &mut D) -> Result<(), D::Error> {
    Circle::new(top_left, DOT_SIZE).into_styled(PrimitiveStyle::with_fill(color)).draw(target)
}

// The text sizes, as the C++ firmware's Montserrat 14, 20 and 48: the
// time in a large numeric font (its digits, ':' and '-'), the name too.
#[derive(Clone, Copy)]
enum TextSize {
    Small,
    Medium,
    Large,
    Time,
}

fn text<D: DrawTarget<Color = Rgb565>>(
    size: TextSize,
    s: &str,
    at: Point,
    align: HorizontalAlignment,
    color: Rgb565,
    target: &mut D,
) -> Result<(), D::Error> {
    let font = match size {
        TextSize::Small => FontRenderer::new::<fonts::u8g2_font_helvR10_tr>(),
        TextSize::Medium => FontRenderer::new::<fonts::u8g2_font_helvB14_tr>(),
        TextSize::Large => FontRenderer::new::<fonts::u8g2_font_helvB24_tr>(),
        TextSize::Time => FontRenderer::new::<fonts::u8g2_font_logisoso42_tn>(),
    };
    render(&font, s, at, align, color, target)
}

fn render<D: DrawTarget<Color = Rgb565>>(
    font: &FontRenderer,
    s: &str,
    at: Point,
    align: HorizontalAlignment,
    color: Rgb565,
    target: &mut D,
) -> Result<(), D::Error> {
    use u8g2_fonts::Error;
    match font.render_aligned(s, at, VerticalPosition::Top, align, FontColor::Transparent(color), target) {
        Ok(_) | Err(Error::GlyphNotFound(_)) | Err(Error::BackgroundColorNotSupported) => Ok(()),
        Err(Error::DisplayError(e)) => Err(e),
    }
}

/// A picture in memory: the firmware draws into one and sends it to the
/// panel at once (no flicker); the tests look at its pixels.
pub struct Frame {
    pub pixels: Vec<Rgb565>,
}

impl Default for Frame {
    fn default() -> Self {
        Frame { pixels: vec![BLACK; (WIDTH * HEIGHT) as usize] }
    }
}

impl Frame {
    pub fn at(&self, p: Point) -> Rgb565 {
        self.pixels[(p.y as u32 * WIDTH + p.x as u32) as usize]
    }
}

impl OriginDimensions for Frame {
    fn size(&self) -> Size {
        Size::new(WIDTH, HEIGHT)
    }
}

impl DrawTarget for Frame {
    type Color = Rgb565;
    type Error = core::convert::Infallible;

    fn draw_iter<I: IntoIterator<Item = Pixel<Rgb565>>>(&mut self, pixels: I) -> Result<(), Self::Error> {
        for Pixel(p, color) in pixels {
            if (0..WIDTH as i32).contains(&p.x) && (0..HEIGHT as i32).contains(&p.y) {
                self.pixels[(p.y as u32 * WIDTH + p.x as u32) as usize] = color;
            }
        }
        Ok(())
    }
}
