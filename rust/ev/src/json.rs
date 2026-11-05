//! A small JSON reader, for installation profiles: the whole document into
//! values, bounded in depth. (The weather's streaming scanner reads replies
//! too big to hold; a profile is a few KB.)

/// A JSON value. An object keeps its members in order, duplicates and all,
/// for the reader to judge.
#[derive(Clone, PartialEq, Debug)]
pub enum Value {
    Null,
    Bool(bool),
    Number(f64),
    String(String),
    Array(Vec<Value>),
    Object(Vec<(String, Value)>),
}

impl Value {
    pub fn as_str(&self) -> Option<&str> {
        if let Value::String(s) = self { Some(s) } else { None }
    }
    pub fn as_array(&self) -> Option<&[Value]> {
        if let Value::Array(a) = self { Some(a) } else { None }
    }
    pub fn as_object(&self) -> Option<&[(String, Value)]> {
        if let Value::Object(o) = self { Some(o) } else { None }
    }
    /// A whole number in i32's range.
    pub fn as_int(&self) -> Option<i32> {
        match self {
            Value::Number(n) if n.fract() == 0.0 && (i32::MIN as f64..=i32::MAX as f64).contains(n) => Some(*n as i32),
            _ => None,
        }
    }
}

const MAX_DEPTH: usize = 16;

/// `text` as one JSON value, or why not.
pub fn parse(text: &str) -> Result<Value, String> {
    let mut r = Reader { bytes: text.as_bytes(), at: 0 };
    let value = r.value(0)?;
    r.space();
    if r.at != r.bytes.len() {
        return Err(format!("unexpected text at byte {}", r.at));
    }
    Ok(value)
}

struct Reader<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl Reader<'_> {
    fn space(&mut self) {
        while self.bytes.get(self.at).is_some_and(|c| matches!(c, b' ' | b'\t' | b'\r' | b'\n')) {
            self.at += 1;
        }
    }

    fn take(&mut self, c: u8) -> bool {
        self.space();
        let found = self.bytes.get(self.at) == Some(&c);
        self.at += found as usize;
        found
    }

    fn expect(&mut self, c: u8) -> Result<(), String> {
        if self.take(c) { Ok(()) } else { Err(format!("expected '{}' at byte {}", c as char, self.at)) }
    }

    fn word(&mut self, word: &str, value: Value) -> Result<Value, String> {
        if self.bytes[self.at..].starts_with(word.as_bytes()) {
            self.at += word.len();
            Ok(value)
        } else {
            Err(format!("unexpected text at byte {}", self.at))
        }
    }

    fn value(&mut self, depth: usize) -> Result<Value, String> {
        if depth > MAX_DEPTH {
            return Err("nested too deeply".into());
        }
        self.space();
        match self.bytes.get(self.at) {
            Some(b'{') => {
                self.at += 1;
                let mut members = Vec::new();
                if !self.take(b'}') {
                    loop {
                        self.space();
                        let key = self.string()?;
                        self.expect(b':')?;
                        members.push((key, self.value(depth + 1)?));
                        if self.take(b'}') {
                            break;
                        }
                        self.expect(b',')?;
                    }
                }
                Ok(Value::Object(members))
            }
            Some(b'[') => {
                self.at += 1;
                let mut items = Vec::new();
                if !self.take(b']') {
                    loop {
                        items.push(self.value(depth + 1)?);
                        if self.take(b']') {
                            break;
                        }
                        self.expect(b',')?;
                    }
                }
                Ok(Value::Array(items))
            }
            Some(b'"') => self.string().map(Value::String),
            Some(b't') => self.word("true", Value::Bool(true)),
            Some(b'f') => self.word("false", Value::Bool(false)),
            Some(b'n') => self.word("null", Value::Null),
            Some(b'-' | b'0'..=b'9') => self.number(),
            _ => Err(format!("expected a value at byte {}", self.at)),
        }
    }

    fn number(&mut self) -> Result<Value, String> {
        let start = self.at;
        while self
            .bytes
            .get(self.at)
            .is_some_and(|c| c.is_ascii_digit() || matches!(c, b'-' | b'+' | b'.' | b'e' | b'E'))
        {
            self.at += 1;
        }
        let text = std::str::from_utf8(&self.bytes[start..self.at]).unwrap_or("");
        text.parse().map(Value::Number).map_err(|_| format!("bad number at byte {start}"))
    }

    fn hex4(&mut self) -> Result<u32, String> {
        let digits = self.bytes.get(self.at..self.at + 4).ok_or("cut-off \\u escape")?;
        self.at += 4;
        u32::from_str_radix(std::str::from_utf8(digits).unwrap_or("x"), 16).map_err(|_| "bad \\u escape".into())
    }

    fn string(&mut self) -> Result<String, String> {
        if self.bytes.get(self.at) != Some(&b'"') {
            return Err(format!("expected a string at byte {}", self.at));
        }
        self.at += 1;
        let mut out = Vec::new();
        loop {
            let c = *self.bytes.get(self.at).ok_or("cut-off string")?;
            self.at += 1;
            match c {
                b'"' => return String::from_utf8(out).map_err(|_| "a string isn't UTF-8".into()),
                b'\\' => {
                    let e = *self.bytes.get(self.at).ok_or("cut-off string")?;
                    self.at += 1;
                    match e {
                        b'"' | b'\\' | b'/' => out.push(e),
                        b'n' => out.push(b'\n'),
                        b't' => out.push(b'\t'),
                        b'r' => out.push(b'\r'),
                        b'b' => out.push(8),
                        b'f' => out.push(12),
                        b'u' => {
                            // One character from the basic plane: no NUL, no surrogates.
                            let c = char::from_u32(self.hex4()?).filter(|&c| c != '\0').ok_or("bad \\u escape")?;
                            out.extend(c.encode_utf8(&mut [0; 4]).bytes());
                        }
                        _ => return Err("bad escape".into()),
                    }
                }
                c if c < 0x20 => return Err("a control character in a string".into()),
                c => out.push(c),
            }
        }
    }
}
