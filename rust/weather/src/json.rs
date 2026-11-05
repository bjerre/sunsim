//! A streaming JSON scanner.

/// A scalar, with where it sits: its key (for an array element, the
/// array's) and the key of the object holding that.
#[derive(Clone, Copy, Debug)]
pub struct Scalar<'a> {
    pub key: &'a str,
    pub parent: &'a str,
    pub text: &'a str, // a string's contents, or a literal as written: 42, -1.5, true, null
    pub is_string: bool,
}

impl Scalar<'_> {
    pub fn number(&self) -> Option<f64> {
        let t = self.text;
        // Only what a JSON number can be (Rust's parse also takes "inf", "nan", "+1").
        if self.is_string
            || t.is_empty()
            || !t.bytes().all(|c| c.is_ascii_digit() || b"-.eE+".contains(&c))
            || t.starts_with('+')
        {
            return None;
        }
        t.parse().ok()
    }
}

/// What a JsonScanner reports to.
pub trait JsonSink {
    fn scalar(&mut self, s: &Scalar);
    /// An object closes: the key it was under ("" at the top).
    fn object_end(&mut self, _key: &str) {}
}

#[derive(Clone, Debug)]
struct Frame {
    key: Vec<u8>,
    array: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum State {
    Between,
    String,
    Escape,
    Unicode,
    Literal,
}

// The longest prefix of `bytes` that's UTF-8: text cut to fit can end mid-character.
fn utf8(bytes: &[u8]) -> &str {
    match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(e) => std::str::from_utf8(&bytes[..e.valid_up_to()]).unwrap_or_default(),
    }
}

/// Reads JSON a chunk at a time as it arrives, in memory bounded by its
/// nesting, and reports each scalar with where it sits: its key (for an
/// array element, the array's) and the key of the object holding that.
///
/// ```text
/// {"current": {"cloud_cover": 75}}      -> key "cloud_cover", parent "current"
/// {"hourly": {"time": [1, 2]}}          -> key "time", parent "hourly", twice
/// ```
///
/// and each object as it closes, by its key. Strings are unescaped and cut
/// to MAX_TEXT bytes. Enough for the weather services' replies; it doesn't
/// validate.
#[derive(Debug)]
pub struct JsonScanner {
    frames: Vec<Frame>,
    text: Vec<u8>,
    key: Vec<u8>,     // the member's key, once read
    expect_key: bool, // in an object, before the colon
    state: State,
    unicode_left: u8,
}

impl Default for JsonScanner {
    fn default() -> Self {
        JsonScanner::new()
    }
}

impl JsonScanner {
    pub const MAX_TEXT: usize = 64;

    pub fn new() -> Self {
        JsonScanner {
            frames: Vec::new(),
            text: Vec::new(),
            key: Vec::new(),
            expect_key: false,
            state: State::Between,
            unicode_left: 0,
        }
    }

    pub fn feed(&mut self, chunk: &[u8], sink: &mut impl JsonSink) {
        for &c in chunk {
            self.character(c, sink);
        }
    }

    fn append(&mut self, c: u8) {
        if self.text.len() < Self::MAX_TEXT {
            self.text.push(c);
        }
    }

    fn character(&mut self, c: u8, sink: &mut impl JsonSink) {
        match self.state {
            State::String => {
                match c {
                    b'\\' => self.state = State::Escape,
                    b'"' => {
                        self.state = State::Between;
                        self.token(true, sink);
                    }
                    _ => self.append(c),
                }
                return;
            }
            State::Escape => {
                self.state = State::String;
                match c {
                    b'n' => self.append(b'\n'),
                    b't' => self.append(b'\t'),
                    b'u' => {
                        // \uXXXX: beyond what's needed here
                        self.append(b'?');
                        self.state = State::Unicode;
                        self.unicode_left = 4;
                    }
                    _ => self.append(c), // \" \\ \/ (and \b \f \r, near enough)
                }
                return;
            }
            State::Unicode => {
                self.unicode_left -= 1;
                if self.unicode_left == 0 {
                    self.state = State::String;
                }
                return;
            }
            State::Literal => {
                if c.is_ascii_alphanumeric() || matches!(c, b'-' | b'+' | b'.') {
                    self.append(c);
                    return;
                }
                self.state = State::Between;
                self.token(false, sink); // and c is structure: handled below
            }
            State::Between => {}
        }

        match c {
            b'{' => self.open(false),
            b'[' => self.open(true),
            b'}' | b']' => self.close(sink),
            b':' => self.expect_key = false,
            b',' => self.expect_key = self.frames.last().is_some_and(|f| !f.array),
            b'"' => {
                self.text.clear();
                self.state = State::String;
            }
            _ if !c.is_ascii_whitespace() => {
                self.text.clear();
                self.text.push(c);
                self.state = State::Literal;
            }
            _ => {}
        }
    }

    // The key a value here goes by: the member's in an object, the array's in an array.
    fn key_here(&self) -> &[u8] {
        match self.frames.last() {
            None => &[],
            Some(f) if f.array => &f.key,
            Some(_) => &self.key,
        }
    }

    fn open(&mut self, array: bool) {
        let key = self.key_here().to_vec();
        self.frames.push(Frame { key, array });
        self.expect_key = !array;
    }

    fn close(&mut self, sink: &mut impl JsonSink) {
        let Some(frame) = self.frames.pop() else { return };
        if !frame.array {
            sink.object_end(utf8(&frame.key));
        }
        self.expect_key = false;
    }

    fn token(&mut self, is_string: bool, sink: &mut impl JsonSink) {
        if self.expect_key {
            self.key.clone_from(&self.text);
            return;
        }
        let Some(last) = self.frames.last() else { return };
        // In an array, the parent is the object holding the array: the frame below.
        let parent: &[u8] = if !last.array {
            &last.key
        } else if self.frames.len() >= 2 {
            &self.frames[self.frames.len() - 2].key
        } else {
            &[]
        };
        sink.scalar(&Scalar { key: utf8(self.key_here()), parent: utf8(parent), text: utf8(&self.text), is_string });
    }
}
