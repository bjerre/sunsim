// Builds a bounded run-time installation from the role catalogue. Names are
// owned here because ZoneSpec keeps string_views into them.

export module sunsim.ev:installation_profile;

#if !defined(__EMSCRIPTEN__)
import std.compat;
#endif
import :installation;
import :topology;
import :types;

export namespace sunsim::ev {

class InstallationProfileBuilder {
public:
    [[nodiscard]] bool begin(int zone_count);
    [[nodiscard]] bool setZone(int index, std::string_view name, int start_minute, int end_minute);
    [[nodiscard]] bool addMember(int zone_index, int role_index, int look_index, int peak_percent);
    [[nodiscard]] bool setButton(int zone_index, int role_index);
    [[nodiscard]] bool addWeatherRole(int zone_index, int role_index, int effect);
    [[nodiscard]] bool finish();
    [[nodiscard]] bool parseJson(std::string_view json);

    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] const Installation &installation() const noexcept { return installation_; }

private:
    Installation installation_{};
    std::array<std::array<char, 32>, kMaxZones> names_{};
    std::array<bool, kMaxZones> configured_{};
    PerRole<bool> assigned_{};
    PerRole<std::optional<ZoneId>> member_zone_{};
    bool button_assigned_ = false;
    bool begun_ = false;
    bool ready_ = false;
};

} // namespace sunsim::ev

namespace sunsim::ev {

namespace {

class ProfileJsonReader {
public:
    explicit ProfileJsonReader(std::string_view input) : input_{input} {}

    bool parse(InstallationProfileBuilder &builder)
    {
        whitespace();
        if (!take('{')) {
            return false;
        }
        bool version_seen = false;
        bool zones_seen = false;
        int version = 0;
        std::size_t zones_start = 0;
        std::size_t zones_end = 0;
        whitespace();
        if (!take('}')) {
            do {
                std::array<char, 32> key{};
                std::size_t key_size = 0;
                if (!readString(key.data(), key.size(), key_size) || !take(':')) {
                    return false;
                }
                const std::string_view name{key.data(), key_size};
                if (name == "schemaVersion") {
                    if (version_seen || !readInteger(version)) {
                        return false;
                    }
                    version_seen = true;
                } else if (name == "zones") {
                    if (zones_seen) {
                        return false;
                    }
                    whitespace();
                    zones_start = position_;
                    if (!skipValue(0)) {
                        return false;
                    }
                    zones_end = position_;
                    zones_seen = true;
                } else {
                    return false;
                }
                whitespace();
                if (take('}')) {
                    break;
                }
                if (!take(',')) {
                    return false;
                }
                whitespace();
            } while (true);
        }
        whitespace();
        if (position_ != input_.size() || !version_seen || version != 1 || !zones_seen) {
            return false;
        }

        ProfileJsonReader counter{input_.substr(zones_start, zones_end - zones_start)};
        int zone_count = 0;
        if (!counter.countZones(zone_count) || !builder.begin(zone_count)) {
            return false;
        }
        ProfileJsonReader zones{input_.substr(zones_start, zones_end - zones_start)};
        return zones.readZones(builder) && builder.finish();
    }

private:
    struct Member {
        int role = -1;
        int look = -1;
        int peak = 100;
        bool has_role = false;
        bool has_look = false;
    };

    struct Zone {
        std::array<char, 32> name{};
        std::size_t name_size = 0;
        int start = 0;
        int end = 0;
        bool has_name = false;
        bool has_start = false;
        bool has_end = false;
        bool has_members = false;
        std::array<Member, kRoleCount> members{};
        std::size_t member_count = 0;
        int button = -1;
        std::array<int, kRoleCount> precipitation{};
        std::size_t precipitation_count = 0;
        std::array<int, kRoleCount> alerts{};
        std::size_t alert_count = 0;
    };

    void whitespace()
    {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\r' || input_[position_] == '\n')) {
            ++position_;
        }
    }

    bool take(char expected)
    {
        whitespace();
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool appendUtf8(uint32_t codepoint, char *output, std::size_t capacity, std::size_t &size)
    {
        if (codepoint == 0 || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
            return false;
        }
        std::array<char, 4> encoded{};
        std::size_t count = 0;
        if (codepoint <= 0x7f) {
            encoded[count++] = static_cast<char>(codepoint);
        } else if (codepoint <= 0x7ff) {
            encoded[count++] = static_cast<char>(0xc0 | (codepoint >> 6));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        } else if (codepoint <= 0xffff) {
            encoded[count++] = static_cast<char>(0xe0 | (codepoint >> 12));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        } else {
            encoded[count++] = static_cast<char>(0xf0 | (codepoint >> 18));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
            encoded[count++] = static_cast<char>(0x80 | (codepoint & 0x3f));
        }
        if (count >= capacity - size) {
            return false;
        }
        std::ranges::copy(std::span{encoded}.first(count), output + size);
        size += count;
        output[size] = '\0';
        return true;
    }

    bool hex4(uint32_t &value)
    {
        if (input_.size() - position_ < 4) {
            return false;
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char digit = input_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<uint32_t>(digit - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool readString(char *output, std::size_t capacity, std::size_t &size)
    {
        whitespace();
        if (!take('"')) {
            return false;
        }
        size = 0;
        output[0] = '\0';
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') {
                return true;
            }
            if (byte < 0x20) {
                return false;
            }
            if (byte != '\\') {
                if (size + 1 >= capacity) {
                    return false;
                }
                output[size++] = static_cast<char>(byte);
                output[size] = '\0';
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escape = input_[position_++];
            switch (escape) {
            case '"': case '\\': case '/':
                if (!appendUtf8(static_cast<unsigned char>(escape), output, capacity, size)) {
                    return false;
                }
                break;
            case 'b': case 'f': case 'n': case 'r': case 't':
                if (!appendUtf8(escape == 'b' ? '\b' : escape == 'f' ? '\f' :
                               escape == 'n' ? '\n' : escape == 'r' ? '\r' : '\t',
                               output, capacity, size)) {
                    return false;
                }
                break;
            case 'u': {
                uint32_t codepoint = 0;
                if (!hex4(codepoint)) {
                    return false;
                }
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') {
                        return false;
                    }
                    position_ += 2;
                    uint32_t low = 0;
                    if (!hex4(low) || low < 0xdc00 || low > 0xdfff) {
                        return false;
                    }
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                }
                if (!appendUtf8(codepoint, output, capacity, size)) {
                    return false;
                }
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool skipString()
    {
        whitespace();
        if (!take('"')) {
            return false;
        }
        while (position_ < input_.size()) {
            const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
            if (byte == '"') {
                return true;
            }
            if (byte < 0x20) {
                return false;
            }
            if (byte != '\\') {
                continue;
            }
            if (position_ >= input_.size()) {
                return false;
            }
            const char escape = input_[position_++];
            if (escape == '"' || escape == '\\' || escape == '/' || escape == 'b' ||
                escape == 'f' || escape == 'n' || escape == 'r' || escape == 't') {
                continue;
            }
            if (escape != 'u') {
                return false;
            }
            uint32_t codepoint = 0;
            if (!hex4(codepoint)) {
                return false;
            }
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (input_.size() - position_ < 6 || input_[position_] != '\\' ||
                    input_[position_ + 1] != 'u') {
                    return false;
                }
                position_ += 2;
                uint32_t low = 0;
                if (!hex4(low) || low < 0xdc00 || low > 0xdfff) {
                    return false;
                }
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                return false;
            }
        }
        return false;
    }

    bool readInteger(int &value)
    {
        whitespace();
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return false;
            }
        } else {
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' || input_[position_] == 'E')) {
            return false;
        }
        const bool negative = input_[start] == '-';
        const uint64_t limit = static_cast<uint64_t>(std::numeric_limits<int>::max()) +
                               static_cast<uint64_t>(negative);
        uint64_t magnitude = 0;
        for (std::size_t i = start + static_cast<std::size_t>(negative); i < position_; ++i) {
            const uint64_t digit = static_cast<uint64_t>(input_[i] - '0');
            if (magnitude > (limit - digit) / 10) {
                return false;
            }
            magnitude = magnitude * 10 + digit;
        }
        if (negative && magnitude == limit) {
            value = std::numeric_limits<int>::min();
        } else {
            const int parsed = static_cast<int>(magnitude);
            value = negative ? -parsed : parsed;
        }
        return true;
    }

    bool skipNumber()
    {
        whitespace();
        const std::size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') {
            ++position_;
        }
        if (position_ >= input_.size() || input_[position_] < '0' || input_[position_] > '9') {
            return false;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                return false;
            }
        } else {
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                return false;
            }
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const std::size_t digits = position_;
            while (position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                return false;
            }
        }
        return position_ != start;
    }

    bool skipValue(unsigned depth)
    {
        if (depth > 16) {
            return false;
        }
        whitespace();
        if (position_ >= input_.size()) {
            return false;
        }
        if (input_[position_] == '"') {
            return skipString();
        }
        if (input_[position_] == '{') {
            ++position_;
            whitespace();
            if (position_ < input_.size() && input_[position_] == '}') {
                ++position_;
                return true;
            }
            do {
                if (!skipString() || !take(':') || !skipValue(depth + 1)) {
                    return false;
                }
                whitespace();
                if (take('}')) {
                    return true;
                }
                if (!take(',')) {
                    return false;
                }
            } while (true);
        }
        if (input_[position_] == '[') {
            ++position_;
            whitespace();
            if (position_ < input_.size() && input_[position_] == ']') {
                ++position_;
                return true;
            }
            do {
                if (!skipValue(depth + 1)) {
                    return false;
                }
                whitespace();
                if (take(']')) {
                    return true;
                }
                if (!take(',')) {
                    return false;
                }
            } while (true);
        }
        for (const std::string_view literal : {"true", "false", "null"}) {
            if (input_.substr(position_).starts_with(literal)) {
                position_ += literal.size();
                return true;
            }
        }
        return skipNumber();
    }

    bool countZones(int &count)
    {
        if (!take('[')) {
            return false;
        }
        count = 0;
        whitespace();
        if (take(']')) {
            return position_ == input_.size();
        }
        do {
            if (!skipValue(0) || ++count > static_cast<int>(kMaxZones)) {
                return false;
            }
            whitespace();
            if (take(']')) {
                return position_ == input_.size();
            }
            if (!take(',')) {
                return false;
            }
        } while (true);
    }

    bool roleIndex(int &index)
    {
        std::array<char, 32> name{};
        std::size_t size = 0;
        if (!readString(name.data(), name.size(), size)) {
            return false;
        }
        const auto role = parseRole(std::string_view{name.data(), size});
        if (!role) {
            return false;
        }
        index = static_cast<int>(slot(*role));
        return true;
    }

    bool readRoleArray(std::array<int, kRoleCount> &roles, std::size_t &count)
    {
        if (!take('[')) {
            return false;
        }
        count = 0;
        whitespace();
        if (take(']')) {
            return true;
        }
        do {
            int role = -1;
            if (count >= roles.size() || !roleIndex(role)) {
                return false;
            }
            roles[count++] = role;
            whitespace();
            if (take(']')) {
                return true;
            }
            if (!take(',')) {
                return false;
            }
        } while (true);
    }

    bool readMembers(Zone &zone)
    {
        if (!take('[')) {
            return false;
        }
        zone.member_count = 0;
        whitespace();
        if (take(']')) {
            return true;
        }
        do {
            if (zone.member_count >= zone.members.size() || !take('{')) {
                return false;
            }
            Member member{};
            unsigned fields = 0;
            whitespace();
            if (!take('}')) {
                do {
                    std::array<char, 32> key{};
                    std::size_t key_size = 0;
                    if (!readString(key.data(), key.size(), key_size) || !take(':')) {
                        return false;
                    }
                    const std::string_view name{key.data(), key_size};
                    if (name == "role") {
                        if ((fields & 1u) || !roleIndex(member.role)) return false;
                        fields |= 1u;
                        member.has_role = true;
                    } else if (name == "look") {
                        if ((fields & 2u)) return false;
                        std::array<char, 32> look{};
                        std::size_t look_size = 0;
                        if (!readString(look.data(), look.size(), look_size)) return false;
                        const std::string_view value{look.data(), look_size};
                        member.look = value == "sun-curve" ? 0 : value == "work-curve" ? 1 :
                                      value == "on-off" ? 2 : -1;
                        if (member.look < 0) return false;
                        fields |= 2u;
                        member.has_look = true;
                    } else if (name == "peakPercent") {
                        if ((fields & 4u) || !readInteger(member.peak)) return false;
                        fields |= 4u;
                    } else {
                        return false;
                    }
                    whitespace();
                    if (take('}')) break;
                    if (!take(',')) return false;
                } while (true);
            }
            if (!member.has_role || !member.has_look || member.peak < 1 || member.peak > 100) {
                return false;
            }
            zone.members[zone.member_count++] = member;
            whitespace();
            if (take(']')) return true;
            if (!take(',')) return false;
        } while (true);
    }

    bool readWeather(Zone &zone)
    {
        if (!take('{')) {
            return false;
        }
        unsigned fields = 0;
        whitespace();
        if (take('}')) {
            return true;
        }
        do {
            std::array<char, 32> key{};
            std::size_t key_size = 0;
            if (!readString(key.data(), key.size(), key_size) || !take(':')) {
                return false;
            }
            const std::string_view name{key.data(), key_size};
            if (name == "precipitation") {
                if ((fields & 1u) || !readRoleArray(zone.precipitation, zone.precipitation_count)) {
                    return false;
                }
                fields |= 1u;
            } else if (name == "alerts") {
                if ((fields & 2u) || !readRoleArray(zone.alerts, zone.alert_count)) {
                    return false;
                }
                fields |= 2u;
            } else {
                return false;
            }
            whitespace();
            if (take('}')) return true;
            if (!take(',')) return false;
        } while (true);
    }

    bool applyZone(Zone &zone, int index, InstallationProfileBuilder &builder)
    {
        if (!zone.has_name || !zone.has_start || !zone.has_end || !zone.has_members ||
            !builder.setZone(index, std::string_view{zone.name.data(), zone.name_size},
                             zone.start, zone.end)) {
            return false;
        }
        for (std::size_t i = 0; i < zone.member_count; ++i) {
            const Member &member = zone.members[i];
            if (!builder.addMember(index, member.role, member.look, member.peak)) {
                return false;
            }
        }
        if (zone.button >= 0 && !builder.setButton(index, zone.button)) {
            return false;
        }
        for (std::size_t i = 0; i < zone.precipitation_count; ++i) {
            if (!builder.addWeatherRole(index, zone.precipitation[i], 0)) {
                return false;
            }
        }
        for (std::size_t i = 0; i < zone.alert_count; ++i) {
            if (!builder.addWeatherRole(index, zone.alerts[i], 1)) {
                return false;
            }
        }
        return true;
    }

    bool readZone(int index, InstallationProfileBuilder &builder)
    {
        if (!take('{')) {
            return false;
        }
        Zone zone{};
        unsigned fields = 0;
        whitespace();
        if (!take('}')) {
            do {
                std::array<char, 32> key{};
                std::size_t key_size = 0;
                if (!readString(key.data(), key.size(), key_size) || !take(':')) {
                    return false;
                }
                const std::string_view name{key.data(), key_size};
                if (name == "name") {
                    if ((fields & 1u) || !readString(zone.name.data(), zone.name.size(), zone.name_size)) return false;
                    fields |= 1u;
                    zone.has_name = true;
                } else if (name == "startMinute") {
                    if ((fields & 2u) || !readInteger(zone.start)) return false;
                    fields |= 2u;
                    zone.has_start = true;
                } else if (name == "endMinute") {
                    if ((fields & 4u) || !readInteger(zone.end)) return false;
                    fields |= 4u;
                    zone.has_end = true;
                } else if (name == "members") {
                    if ((fields & 8u) || !readMembers(zone)) return false;
                    fields |= 8u;
                    zone.has_members = true;
                } else if (name == "buttonRole") {
                    if ((fields & 16u) || !roleIndex(zone.button)) return false;
                    fields |= 16u;
                } else if (name == "weather") {
                    if ((fields & 32u) || !readWeather(zone)) return false;
                    fields |= 32u;
                } else {
                    return false;
                }
                whitespace();
                if (take('}')) break;
                if (!take(',')) return false;
            } while (true);
        }
        return applyZone(zone, index, builder);
    }

    bool readZones(InstallationProfileBuilder &builder)
    {
        if (!take('[')) {
            return false;
        }
        int index = 0;
        whitespace();
        if (!take(']')) {
            do {
                if (!readZone(index++, builder)) {
                    return false;
                }
                whitespace();
                if (take(']')) break;
                if (!take(',')) return false;
            } while (true);
        }
        whitespace();
        return position_ == input_.size() && index >= 1 &&
               index <= static_cast<int>(kMaxZones);
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

} // namespace

bool InstallationProfileBuilder::parseJson(std::string_view json)
{
    if (json.empty() || json.find('\0') != std::string_view::npos) {
        return false;
    }
    return ProfileJsonReader{json}.parse(*this);
}

bool InstallationProfileBuilder::begin(int zone_count)
{
    if (zone_count < 1 || zone_count > static_cast<int>(kMaxZones)) {
        return false;
    }
    installation_ = {};
    names_ = {};
    configured_ = {};
    assigned_ = {};
    member_zone_ = {};
    button_assigned_ = false;
    installation_.zone_count = static_cast<std::size_t>(zone_count);
    begun_ = true;
    ready_ = false;
    return true;
}

bool InstallationProfileBuilder::setZone(int index, std::string_view name, int start_minute, int end_minute)
{
    using namespace std::chrono;
    if (!begun_ || index < 0 || index >= static_cast<int>(installation_.zone_count) || name.empty() ||
        name.size() >= names_[0].size() || start_minute < 0 || start_minute >= end_minute ||
        end_minute > 24 * 60) {
        return false;
    }
    for (std::size_t i = 0; i < installation_.zone_count; ++i) {
        if (i != static_cast<std::size_t>(index) && installation_.zones[i].name == name) {
            return false;
        }
    }
    auto &storage = names_[static_cast<std::size_t>(index)];
    std::ranges::copy(name, storage.begin());
    auto &zone = installation_.zones[static_cast<std::size_t>(index)];
    zone.name = {storage.data(), name.size()};
    zone.gate = {Window{.hours = DaySpan{minutes{start_minute}, minutes{end_minute}}, .days = kEveryDay}};
    configured_[static_cast<std::size_t>(index)] = true;
    return true;
}

bool InstallationProfileBuilder::addMember(int zone_index, int role_index, int look_index, int peak_percent)
{
    if (!begun_ || zone_index < 0 || zone_index >= static_cast<int>(installation_.zone_count) ||
        role_index < 0 || role_index >= static_cast<int>(kRoleCount) || peak_percent < 1 ||
        peak_percent > 100) {
        return false;
    }
    const Role role = kRoles[static_cast<std::size_t>(role_index)];
    if (!isLight(role) || assigned_[role]) {
        return false;
    }

    Look look;
    switch (look_index) {
    case 0:
        if (kindOf(role) != Kind::ColorBulb) {
            return false;
        }
        look = SunCurve{};
        break;
    case 1:
        if (kindOf(role) != Kind::ColorBulb) {
            return false;
        }
        look = WorkCurve{};
        break;
    case 2:
        if (kindOf(role) != Kind::Plug) {
            return false;
        }
        look = OnOffOnly{};
        break;
    default:
        return false;
    }

    auto &zone = installation_.zones[static_cast<std::size_t>(zone_index)];
    if (!zone.lights.push_back({.lights = roleName(role), .look = std::move(look),
                                .peak = static_cast<uint8_t>(peak_percent)})) {
        return false;
    }
    assigned_[role] = true;
    member_zone_[role] = ZoneId{static_cast<uint8_t>(zone_index)};
    return true;
}

bool InstallationProfileBuilder::setButton(int zone_index, int role_index)
{
    if (!begun_ || zone_index < 0 || zone_index >= static_cast<int>(installation_.zone_count) ||
        role_index < 0 || role_index >= static_cast<int>(kRoleCount) || button_assigned_) {
        return false;
    }
    const Role role = kRoles[static_cast<std::size_t>(role_index)];
    if (!isButton(role)) {
        return false;
    }
    if (!installation_.zones[static_cast<std::size_t>(zone_index)].controls.push_back(
            Controls{.by = roleName(role), .single = Mode::Daylight,
                     .twice = Mode::RedAlert, .hold = Mode::Off})) {
        return false;
    }
    button_assigned_ = true;
    return true;
}

bool InstallationProfileBuilder::addWeatherRole(int zone_index, int role_index, int effect)
{
    if (!begun_ || zone_index < 0 || zone_index >= static_cast<int>(installation_.zone_count) ||
        role_index < 0 || role_index >= static_cast<int>(kRoleCount) || (effect != 0 && effect != 1)) {
        return false;
    }
    const Role role = kRoles[static_cast<std::size_t>(role_index)];
    if (!assigned_[role] || member_zone_[role] != ZoneId{static_cast<uint8_t>(zone_index)} ||
        !caps(role).color) {
        return false;
    }
    auto &zone = installation_.zones[static_cast<std::size_t>(zone_index)];
    if (!zone.weather) {
        zone.weather = Weather{};
    }
    auto &selection = effect == 0 ? zone.weather->precip : zone.weather->alerts;
    const auto name = roleName(role);
    if (std::ranges::contains(selection, name)) {
        return false;
    }
    return selection.push_back(name);
}

bool InstallationProfileBuilder::finish()
{
    if (!begun_) {
        return false;
    }
    for (const Role role : lights()) {
        if (!assigned_[role]) {
            return false;
        }
    }
    for (std::size_t i = 0; i < installation_.zone_count; ++i) {
        if (!configured_[i] || installation_.zones[i].lights.empty()) {
            return false;
        }
    }
    installation_.topology =
        topologyOf(std::span{installation_.zones}.first(installation_.zone_count));
    ready_ = true;
    begun_ = false;
    return true;
}

} // namespace sunsim::ev
