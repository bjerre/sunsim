//! Time, as the app counts it: steady time since boot (`Instant`, with a
//! signed `Duration`), the local wall-clock time the lights run on
//! (`LocalSeconds`, `LocalDays`, `Weekday`), UTC (`SysSeconds`), and Zigbee
//! transition times (`Transition`). Only the platform layer reads a clock;
//! core code gets time from events, which keeps it testable.

use std::fmt;
use std::ops::{Add, AddAssign, Div, Mul, Neg, Rem, Sub, SubAssign};

/// A span of steady time, in microseconds (the esp_timer's unit). Signed,
/// like `std::chrono::duration`: the difference of two instants either way.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Duration(i64);

impl Duration {
    pub const ZERO: Duration = Duration(0);

    pub const fn micros(n: i64) -> Self {
        Duration(n)
    }
    pub const fn millis(n: i64) -> Self {
        Duration(n * 1_000)
    }
    pub const fn secs(n: i64) -> Self {
        Duration(n * 1_000_000)
    }
    pub const fn minutes(n: i64) -> Self {
        Duration::secs(n * 60)
    }
    pub const fn hours(n: i64) -> Self {
        Duration::minutes(n * 60)
    }
    pub const fn days(n: i64) -> Self {
        Duration::hours(n * 24)
    }

    pub const fn as_micros(self) -> i64 {
        self.0
    }
    /// Whole seconds, rounded down (towards the past).
    pub const fn floor_secs(self) -> i64 {
        self.0.div_euclid(1_000_000)
    }
    /// Whole minutes, rounded towards zero.
    pub const fn whole_minutes(self) -> i64 {
        self.0 / 60_000_000
    }
    pub fn as_secs_f64(self) -> f64 {
        self.0 as f64 / 1e6
    }
    pub fn as_hours_f64(self) -> f64 {
        self.0 as f64 / 3.6e9
    }
}

impl fmt::Debug for Duration {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}us", self.0)
    }
}

impl Add for Duration {
    type Output = Duration;
    fn add(self, rhs: Duration) -> Duration {
        Duration(self.0 + rhs.0)
    }
}
impl Sub for Duration {
    type Output = Duration;
    fn sub(self, rhs: Duration) -> Duration {
        Duration(self.0 - rhs.0)
    }
}
impl Neg for Duration {
    type Output = Duration;
    fn neg(self) -> Duration {
        Duration(-self.0)
    }
}
impl Mul<i64> for Duration {
    type Output = Duration;
    fn mul(self, rhs: i64) -> Duration {
        Duration(self.0 * rhs)
    }
}
impl Div<i64> for Duration {
    type Output = Duration;
    fn div(self, rhs: i64) -> Duration {
        Duration(self.0 / rhs)
    }
}
/// How many whole `rhs` fit (towards zero), as `std::chrono`'s does.
impl Div for Duration {
    type Output = i64;
    fn div(self, rhs: Duration) -> i64 {
        self.0 / rhs.0
    }
}
impl Rem for Duration {
    type Output = Duration;
    fn rem(self, rhs: Duration) -> Duration {
        Duration(self.0 % rhs.0)
    }
}
impl AddAssign for Duration {
    fn add_assign(&mut self, rhs: Duration) {
        self.0 += rhs.0;
    }
}

/// Steady time since boot (the esp_timer on the target). Only the platform
/// layer calls a clock; core code gets instants from events.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Instant(i64);

impl Instant {
    pub const ZERO: Instant = Instant(0);
    pub const MAX: Instant = Instant(i64::MAX);

    pub const fn from_boot(since: Duration) -> Self {
        Instant(since.0)
    }
    pub const fn since_boot(self) -> Duration {
        Duration(self.0)
    }
}

impl fmt::Debug for Instant {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "T+{}us", self.0)
    }
}

impl Add<Duration> for Instant {
    type Output = Instant;
    fn add(self, rhs: Duration) -> Instant {
        Instant(self.0 + rhs.0)
    }
}
impl Sub<Duration> for Instant {
    type Output = Instant;
    fn sub(self, rhs: Duration) -> Instant {
        Instant(self.0 - rhs.0)
    }
}
impl Sub for Instant {
    type Output = Duration;
    fn sub(self, rhs: Instant) -> Duration {
        Duration(self.0 - rhs.0)
    }
}
impl AddAssign<Duration> for Instant {
    fn add_assign(&mut self, rhs: Duration) {
        self.0 += rhs.0;
    }
}
impl SubAssign<Duration> for Instant {
    fn sub_assign(&mut self, rhs: Duration) {
        self.0 -= rhs.0;
    }
}

/// Zigbee transition times: tenths of a second.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Transition(pub u16);

impl Transition {
    pub const fn tenths(n: u16) -> Self {
        Transition(n)
    }
    pub const fn secs(n: u16) -> Self {
        Transition(n * 10)
    }
    /// `d` in tenths, rounded towards zero.
    pub const fn of(d: Duration) -> Self {
        Transition((d.0 / 100_000) as u16)
    }
    pub const fn duration(self) -> Duration {
        Duration::millis(self.0 as i64 * 100)
    }
}

// ---- The calendar ---------------------------------------------------------

/// A day of the week, as `std::chrono::weekday::c_encoding()`: 0 = Sunday.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct Weekday(pub u8);

impl Weekday {
    pub const SUNDAY: Weekday = Weekday(0);
    pub const MONDAY: Weekday = Weekday(1);
    pub const TUESDAY: Weekday = Weekday(2);
    pub const WEDNESDAY: Weekday = Weekday(3);
    pub const THURSDAY: Weekday = Weekday(4);
    pub const FRIDAY: Weekday = Weekday(5);
    pub const SATURDAY: Weekday = Weekday(6);
}

/// Days from 1970-01-01 to `y`-`m`-`d` (proleptic Gregorian; Howard
/// Hinnant's days_from_civil).
pub const fn days_from_civil(y: i32, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y as i64 - 1 } else { y as i64 };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let mp = (m as i64 + 9) % 12;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// The date `days` after 1970-01-01: (year, month 1-12, day 1-31).
pub const fn civil_from_days(days: i64) -> (i32, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + if m <= 2 { 1 } else { 0 };
    (y as i32, m, d)
}

/// Whether `y`-`m`-`d` is a real date.
pub const fn is_valid_date(y: i32, m: u32, d: u32) -> bool {
    if m < 1 || m > 12 || d < 1 {
        return false;
    }
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    let days = match m {
        2 if leap => 29,
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    };
    d <= days
}

/// A local date: days since 1970-01-01 where the lights are.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct LocalDays(pub i64);

impl LocalDays {
    pub const fn ymd(y: i32, m: u32, d: u32) -> Self {
        LocalDays(days_from_civil(y, m, d))
    }
    pub const fn weekday(self) -> Weekday {
        // 1970-01-01 was a Thursday.
        Weekday((self.0 + 4).rem_euclid(7) as u8)
    }
    pub const fn civil(self) -> (i32, u32, u32) {
        civil_from_days(self.0)
    }
}

/// A local wall-clock time: seconds since 1970-01-01 00:00 where the lights are.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct LocalSeconds(pub i64);

impl LocalSeconds {
    pub const fn day(self) -> LocalDays {
        LocalDays(self.0.div_euclid(86_400))
    }
}

impl Add<Duration> for LocalDays {
    type Output = LocalSeconds;
    fn add(self, rhs: Duration) -> LocalSeconds {
        LocalSeconds(self.0 * 86_400) + rhs
    }
}
/// Plus `rhs`, in whole seconds (rounded down).
impl Add<Duration> for LocalSeconds {
    type Output = LocalSeconds;
    fn add(self, rhs: Duration) -> LocalSeconds {
        LocalSeconds(self.0 + rhs.floor_secs())
    }
}
impl AddAssign<Duration> for LocalSeconds {
    fn add_assign(&mut self, rhs: Duration) {
        *self = *self + rhs;
    }
}
impl Sub<LocalDays> for LocalSeconds {
    type Output = Duration;
    fn sub(self, rhs: LocalDays) -> Duration {
        Duration::secs(self.0 - rhs.0 * 86_400)
    }
}

/// A UTC time: seconds since the Unix epoch.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Hash, Debug)]
pub struct SysSeconds(pub i64);

impl Add<Duration> for SysSeconds {
    type Output = SysSeconds;
    fn add(self, rhs: Duration) -> SysSeconds {
        SysSeconds(self.0 + rhs.floor_secs())
    }
}
impl Sub<Duration> for SysSeconds {
    type Output = SysSeconds;
    fn sub(self, rhs: Duration) -> SysSeconds {
        SysSeconds(self.0 - rhs.floor_secs())
    }
}
impl Sub for SysSeconds {
    type Output = Duration;
    fn sub(self, rhs: SysSeconds) -> Duration {
        Duration::secs(self.0 - rhs.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_calendar_round_trips() {
        for days in [-1_000_000, -1, 0, 1, 19_000, 20_000, 1_000_000] {
            let (y, m, d) = civil_from_days(days);
            assert_eq!(days_from_civil(y, m, d), days);
        }
        assert_eq!(LocalDays::ymd(2022, 6, 21).weekday(), Weekday::TUESDAY);
        assert!(!is_valid_date(2026, 13, 1) && !is_valid_date(2026, 2, 29) && is_valid_date(2024, 2, 29));
    }
}
