//! The status display's picture: what the 240x135 LCD shows - a sky that
//! follows the sun (the day's arc, the sun where it is, a cloud, rain or
//! snow, a skyline), the time, the mode, the day, sunrise and sunset, and a
//! dot per device - from presentation's StatusView, in the LCD's colors.
//! Drawn with embedded-graphics into a Frame (the firmware sends it to the
//! panel at once; the tests look at its pixels). A port of the C++
//! platform's status_display.cppm (its LVGL screen), minus the panel.

use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{Circle, ContainsPoint, Rectangle, RoundedRectangle};
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
const DOTS_TOP_Y: i32 = 118;

const _: () = {
    let mut width = 0;
    let mut g = 0;
    while g < LAYOUT.len() {
        width += LABEL_CELL + LAYOUT[g].count as i32 * DOT_PITCH + GROUP_GAP;
        g += 1;
    }
    assert!(DOTS_LEFT + width - GROUP_GAP <= WIDTH as i32, "the device dots don't fit in one row (see LAYOUT)");
};

pub const fn rgb(hex: u32) -> Rgb565 {
    Rgb565::new(((hex >> 19) & 0x1F) as u8, ((hex >> 10) & 0x3F) as u8, ((hex >> 3) & 0x1F) as u8)
}

pub const BLACK: Rgb565 = rgb(0x000000);
pub const WHITE: Rgb565 = rgb(0xFFFFFF);
pub const LABEL_GRAY: Rgb565 = rgb(0xAAAAAA);
pub const ONLINE: Rgb565 = rgb(0x3DDC84); // green
pub const SILENT: Rgb565 = rgb(0xF5A623); // amber
pub const UNPAIRED: Rgb565 = rgb(0x3A3A3A); // dim gray
pub const RED: Rgb565 = rgb(0xFF3030);

pub fn link_color(link: DeviceLink) -> Rgb565 {
    match link {
        DeviceLink::Online => ONLINE,
        DeviceLink::Silent => SILENT,
        DeviceLink::Unpaired => UNPAIRED,
    }
}

/// A tone in the LCD's colors.
pub const fn tone_color(tone: Tone) -> Rgb565 {
    match tone {
        Tone::Muted => rgb(0x888888),
        Tone::Daylight => rgb(0xFFD27F),
        Tone::Tuning => rgb(0x7FD4FF),
        Tone::Advisory => rgb(0xFFE040),
        Tone::Watch => rgb(0xFF9A30),
        Tone::Warning => RED,
        Tone::Sky => rgb(0x9AB8D8),
    }
}

/// The sky's five bands, top to bottom, by how high the sun is.
pub const SKIES: [[u32; 5]; 4] = [
    [0x050d22, 0x091733, 0x112449, 0x182c50, 0x1d3152], // night
    [0x182044, 0x343b68, 0x765778, 0xd8836c, 0xf5a36d], // twilight
    [0x425b91, 0x7b71a6, 0xc98289, 0xf3a274, 0xffd08a], // golden hour
    [0x2777ae, 0x3489bd, 0x55a5ce, 0x7bbbd2, 0xa6d0d3], // day
];

/// Which of SKIES: night (no clock, or the sun 6 degrees down), twilight, golden hour, day.
pub fn sky_of(v: &StatusView) -> usize {
    match v.sun_elevation_deg {
        _ if !v.time_valid => 0,
        e if e < -6.0 => 0,
        e if e < 0.0 => 1,
        e if e < 12.0 => 2,
        _ => 3,
    }
}

/// Where the sun's disc is centered: across by its bearing (east left),
/// up by its elevation, kept near the horizon.
pub fn sun_at(v: &StatusView) -> Point {
    let x = 120 - (v.sun_azimuth_deg.to_radians().sin() * 101.0) as i32;
    let y = (102 - (v.sun_elevation_deg * 0.40) as i32).clamp(68, 111);
    Point::new(x, y)
}

// A low skyline below the sun's path: (left, width, height).
const BUILDINGS: [(i32, u32, u32); 6] =
    [(8, 24, 16), (39, 19, 11), (66, 31, 22), (105, 23, 14), (137, 28, 19), (174, 48, 13)];

/// What the display shows.
#[derive(Clone, PartialEq, Debug)]
pub struct Screen {
    pub view: StatusView,
}

impl Screen {
    pub fn of(st: &StatusSnapshot) -> Screen {
        Screen { view: StatusView::of(st) }
    }

    pub fn draw(&self, f: &mut Frame) {
        let v = &self.view;
        let sky = SKIES[sky_of(v)];
        for (i, band) in sky.iter().enumerate() {
            f.fill(Rectangle::new(Point::new(0, i as i32 * 21), Size::new(WIDTH, 22)), rgb(*band), 255);
        }
        // The day's arc.
        for i in 0..13 {
            let t = i as f64 / 12.0;
            let (x, y) = (18 + (t * 204.0) as i32, 102 - ((std::f64::consts::PI * t).sin() * 29.0) as i32);
            f.fill_circle(Point::new(x - 2, y - 2), 4, rgb(0xffdba0), 145);
        }
        for (x, w, h) in BUILDINGS {
            f.fill(Rectangle::new(Point::new(x, 133 - h as i32), Size::new(w, h)), rgb(0x172a43), 255);
        }
        f.fill(Rectangle::new(Point::new(0, 103), Size::new(WIDTH, 2)), rgb(0xffd09a), 170);

        // One compact cloud, with rain or snow under it while it falls.
        let show_cloud = v.precipitation != Precip::None || v.cloud_percent.is_some_and(|c| c >= 25);
        if show_cloud {
            let opacity = (100 + v.cloud_percent.unwrap_or(100) as u32 * 155 / 100) as u8;
            let cloud = rgb(0xc7d4de);
            f.fill_rounded(Rectangle::new(Point::new(142, 75), Size::new(35, 8)), 4, cloud, opacity);
            f.fill_circle(Point::new(147, 67), 16, cloud, opacity);
            f.fill_circle(Point::new(158, 63), 20, cloud, opacity);
            f.fill_circle(Point::new(170, 69), 14, cloud, opacity);
            if v.precipitation != Precip::None {
                let drop = rgb(if v.precipitation == Precip::Snow { 0xf1f6ff } else { 0xa8e5ff });
                for i in 0..3 {
                    f.fill_rounded(Rectangle::new(Point::new(151 + i * 10, 83), Size::new(3, 7)), 1, drop, 255);
                }
            }
        }

        if v.time_valid {
            let sun = sun_at(v);
            if v.sun_elevation_deg > -1.0 {
                f.fill_circle(sun - Point::new(12, 12), 25, rgb(0xffbd69), 70);
            }
            let disc = rgb(if v.sun_elevation_deg < -1.0 { 0xb2a18d } else { 0xffdf91 });
            f.fill_circle(sun - Point::new(6, 6), 13, disc, 255);
        }

        f.fill_rounded(Rectangle::new(Point::new(46, 103), Size::new(148, 15)), 7, rgb(0x0c1929), 160);
        f.fill(Rectangle::new(Point::new(0, 118), Size::new(WIDTH, 17)), rgb(0x081321), 255);

        let right = WIDTH as i32 - 6;
        use HorizontalAlignment::{Center, Left, Right};
        text(f, TextSize::Time, &v.time, Point::new(6, 0), Left, WHITE);
        text(f, TextSize::Medium, &v.weekday, Point::new(right, 4), Right, WHITE);
        text(f, TextSize::Small, v.mode, Point::new(right, 29), Right, tone_color(v.mode_tone));
        text(f, TextSize::Small, &v.sky, Point::new(right, 47), Right, tone_color(v.sky_tone));
        text(f, TextSize::Small, &v.day_length, Point::new(7, 54), Left, WHITE);
        text(f, TextSize::Small, &v.sunrise, Point::new(6, 87), Left, WHITE);
        text(f, TextSize::Small, &v.sunset, Point::new(right, 87), Right, WHITE);
        text(f, TextSize::Small, &v.next_event, Point::new(WIDTH as i32 / 2, 104), Center, WHITE);

        let mut x = DOTS_LEFT;
        for g in LAYOUT {
            text(f, TextSize::Small, group_label(g.group), Point::new(x, DOTS_TOP_Y), Left, LABEL_GRAY);
            x += LABEL_CELL;
            for &role in members(g.group) {
                f.fill_circle(Point::new(x, DOTS_TOP_Y + 4), DOT_SIZE, link_color(v.links[role]), 255);
                x += DOT_PITCH;
            }
            x += GROUP_GAP;
        }
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

/// What it shows before the first status: the name, on the night sky.
pub fn draw_splash(f: &mut Frame) {
    for (i, band) in SKIES[0].iter().enumerate() {
        f.fill(Rectangle::new(Point::new(0, i as i32 * 27), Size::new(WIDTH, 27)), rgb(*band), 255);
    }
    text(f, TextSize::Large, "sunsim", Point::new(6, 0), HorizontalAlignment::Left, WHITE);
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

fn text(f: &mut Frame, size: TextSize, s: &str, at: Point, align: HorizontalAlignment, color: Rgb565) {
    let font = match size {
        TextSize::Small => FontRenderer::new::<fonts::u8g2_font_helvR10_tr>(),
        TextSize::Medium => FontRenderer::new::<fonts::u8g2_font_helvB14_tr>(),
        TextSize::Large => FontRenderer::new::<fonts::u8g2_font_helvB24_tr>(),
        TextSize::Time => FontRenderer::new::<fonts::u8g2_font_logisoso42_tn>(),
    };
    // A glyph the font lacks is skipped; a Frame can't fail.
    let _ = font.render_aligned(s, at, VerticalPosition::Top, align, FontColor::Transparent(color), f);
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

// `fg` over `bg`, `alpha` of 255 opaque.
fn blend(bg: Rgb565, fg: Rgb565, alpha: u8) -> Rgb565 {
    let mix = |b: u8, f: u8| ((f as u32 * alpha as u32 + b as u32 * (255 - alpha as u32)) / 255) as u8;
    Rgb565::new(mix(bg.r(), fg.r()), mix(bg.g(), fg.g()), mix(bg.b(), fg.b()))
}

impl Frame {
    pub fn at(&self, p: Point) -> Rgb565 {
        self.pixels[(p.y as u32 * WIDTH + p.x as u32) as usize]
    }

    fn put(&mut self, p: Point, color: Rgb565, alpha: u8) {
        if (0..WIDTH as i32).contains(&p.x) && (0..HEIGHT as i32).contains(&p.y) {
            let i = (p.y as u32 * WIDTH + p.x as u32) as usize;
            self.pixels[i] = if alpha == 255 { color } else { blend(self.pixels[i], color, alpha) };
        }
    }

    fn fill(&mut self, r: Rectangle, color: Rgb565, alpha: u8) {
        for p in r.points() {
            self.put(p, color, alpha);
        }
    }

    fn fill_circle(&mut self, top_left: Point, diameter: u32, color: Rgb565, alpha: u8) {
        let c = Circle::new(top_left, diameter);
        for p in c.bounding_box().points().filter(|&p| c.contains(p)) {
            self.put(p, color, alpha);
        }
    }

    fn fill_rounded(&mut self, r: Rectangle, radius: u32, color: Rgb565, alpha: u8) {
        let rr = RoundedRectangle::with_equal_corners(r, Size::new(radius, radius));
        for p in r.points().filter(|&p| rr.contains(p)) {
            self.put(p, color, alpha);
        }
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
            self.put(p, color, 255);
        }
        Ok(())
    }
}
