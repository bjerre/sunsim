module;
#if defined(__EMSCRIPTEN__)
#include <array>
#include <cerrno>
#include <cstdlib>
#endif

export module sunsim.weather:json;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif

export namespace sunsim::weather {

// Reads JSON a chunk at a time as it arrives, in memory bounded by its
// nesting, and reports each scalar with where it sits: its key (for an
// array element, the array's) and the key of the object holding that.
//
//   {"current": {"cloud_cover": 75}}      -> key "cloud_cover", parent "current"
//   {"hourly": {"time": [1, 2]}}          -> key "time", parent "hourly", twice
//
// and each object as it closes, by its key. Strings are unescaped and cut
// to kMaxText. Enough for the weather services' replies; it doesn't
// validate.
class JsonScanner {
public:
    static constexpr std::size_t kMaxText = 64;

    struct Scalar {
        std::string_view key;
        std::string_view parent;
        std::string_view text;      // a string's contents, or a literal as written: 42, -1.5, true, null
        bool             is_string = false;

        [[nodiscard]] std::optional<double> number() const noexcept;
    };
    using OnScalar    = std::function<void(const Scalar &)>;
    using OnObjectEnd = std::function<void(std::string_view key)>;

    explicit JsonScanner(OnScalar on_scalar, OnObjectEnd on_object_end = {});

    void feed(std::string_view chunk);

private:
    struct Frame {
        std::string key;
        bool        array = false;
    };
    enum class State : uint8_t { Between, String, Escape, Unicode, Literal };

    void character(char c);
    void open(bool array);
    void close();
    void token(bool is_string);
    void append(char c);
    [[nodiscard]] std::string_view keyHere() const noexcept;

    OnScalar           on_scalar_;
    OnObjectEnd        on_object_end_;
    std::vector<Frame> frames_;
    std::string        text_;
    std::string        key_;               // the member's key, once read
    bool               expect_key_ = false; // in an object, before the colon
    State              state_       = State::Between;
    uint8_t            unicode_left_ = 0;
};

} // namespace sunsim::weather

namespace sunsim::weather {

std::optional<double> JsonScanner::Scalar::number() const noexcept
{
#if defined(__EMSCRIPTEN__)
    if (is_string || text.empty() || text.size() > kMaxText || text.front() == '+') {
        return std::nullopt;
    }

    // Match the decimal syntax of from_chars() before strtod(), which accepts
    // a wider language (for example hexadecimal values and leading whitespace).
    std::size_t i = text.front() == '-' ? 1 : 0;
    const std::size_t integer = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        ++i;
    }
    bool has_digits = i != integer;
    if (i < text.size() && text[i] == '.') {
        ++i;
        const std::size_t fraction = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        has_digits = has_digits || i != fraction;
    }
    if (!has_digits) {
        return std::nullopt;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        const std::size_t exponent = i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
        }
        if (i == exponent) {
            return std::nullopt;
        }
    }
    if (i != text.size()) {
        return std::nullopt;
    }

    std::array<char, kMaxText + 1> buffer{};
    std::copy(text.begin(), text.end(), buffer.begin());
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(buffer.data(), &end);
    if (errno == ERANGE || end != buffer.data() + text.size()) {
        return std::nullopt;
    }
    return value;
#else
    double value = 0.0;
    const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (is_string || err != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
#endif
}

JsonScanner::JsonScanner(OnScalar on_scalar, OnObjectEnd on_object_end)
    : on_scalar_(std::move(on_scalar)), on_object_end_(std::move(on_object_end))
{
}

void JsonScanner::feed(std::string_view chunk)
{
    for (const char c : chunk) {
        character(c);
    }
}

void JsonScanner::append(char c)
{
    if (text_.size() < kMaxText) {
        text_ += c;
    }
}

void JsonScanner::character(char c)
{
    switch (state_) {
    case State::String:
        if (c == '\\') {
            state_ = State::Escape;
        } else if (c == '"') {
            state_ = State::Between;
            token(true);
        } else {
            append(c);
        }
        return;
    case State::Escape:
        state_ = State::String;
        switch (c) {
        case 'n':
            append('\n');
            return;
        case 't':
            append('\t');
            return;
        case 'u': // \uXXXX: beyond what's needed here
            append('?');
            state_        = State::Unicode;
            unicode_left_ = 4;
            return;
        default: // \" \\ \/ (and \b \f \r, near enough)
            append(c);
            return;
        }
    case State::Unicode:
        if (--unicode_left_ == 0) {
            state_ = State::String;
        }
        return;
    case State::Literal:
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '+' || c == '.') {
            append(c);
            return;
        }
        state_ = State::Between;
        token(false);
        break; // and c is structure: handled below
    case State::Between:
        break;
    }

    switch (c) {
    case '{':
        open(false);
        break;
    case '[':
        open(true);
        break;
    case '}':
    case ']':
        close();
        break;
    case ':':
        expect_key_ = false;
        break;
    case ',':
        expect_key_ = !frames_.empty() && !frames_.back().array;
        break;
    case '"':
        text_.clear();
        state_ = State::String;
        break;
    default:
        if (!std::isspace(static_cast<unsigned char>(c))) {
            text_.assign(1, c);
            state_ = State::Literal;
        }
        break;
    }
}

// The key a value here goes by: the member's in an object, the array's in an array.
std::string_view JsonScanner::keyHere() const noexcept
{
    if (frames_.empty()) {
        return {};
    }
    return frames_.back().array ? std::string_view{frames_.back().key} : std::string_view{key_};
}

void JsonScanner::open(bool array)
{
    frames_.push_back({.key = std::string{keyHere()}, .array = array});
    expect_key_ = !array;
}

void JsonScanner::close()
{
    if (frames_.empty()) {
        return;
    }
    const Frame frame = std::move(frames_.back());
    frames_.pop_back();
    if (!frame.array && on_object_end_) {
        on_object_end_(frame.key);
    }
    expect_key_ = false;
}

void JsonScanner::token(bool is_string)
{
    if (expect_key_) {
        key_ = text_;
        return;
    }
    if (frames_.empty()) {
        return;
    }
    // In an array, the parent is the object holding the array: the frame below.
    const bool             in_array = frames_.back().array;
    const std::string_view parent   = !in_array ? std::string_view{frames_.back().key}
                                      : frames_.size() >= 2 ? std::string_view{frames_[frames_.size() - 2].key}
                                                            : std::string_view{};
    on_scalar_({.key = keyHere(), .parent = parent, .text = text_, .is_string = is_string});
}

} // namespace sunsim::weather
