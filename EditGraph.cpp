#include "EditGraph.hpp"
#include "Sha256.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <queue>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace rawengine {

TileCache::TileCache(std::size_t max_bytes) : max_bytes_(max_bytes) {}

Tile TileCache::render(const Node& node, std::array<std::uint8_t, 32> signature,
                       Rect bounds) {
    const Key key{signature, {bounds.x, bounds.y, bounds.width, bounds.height}};
    std::uint64_t generation;
    {
        std::lock_guard lock(mutex_);
        if (auto found = entries_.find(key); found != entries_.end()) {
            ++hits_;
            recency_.splice(recency_.begin(), recency_, found->second.recency);
            return found->second.tile;
        }
        ++misses_;
        generation = generation_;
    }
    Tile tile = node.render(bounds);
    const auto pixels = static_cast<std::uint64_t>(bounds.width) * bounds.height;
    if (tile.bounds.x != bounds.x || tile.bounds.y != bounds.y ||
        tile.bounds.width != bounds.width || tile.bounds.height != bounds.height ||
        tile.descriptor != node.output_descriptor() ||
        pixels > std::numeric_limits<std::size_t>::max() / 3 ||
        tile.rgb.size() != static_cast<std::size_t>(pixels) * 3)
        throw std::domain_error("node returned an invalid cache tile");
    constexpr std::size_t entry_charge = 256; // map/list/vector bookkeeping allowance
    if (!pixels || tile.rgb.size() > (max_bytes_ - std::min(max_bytes_, entry_charge)) /
                                     sizeof(float))
        return tile;
    const auto charge = entry_charge + tile.rgb.size() * sizeof(float);
    std::lock_guard lock(mutex_);
    if (generation != generation_) return tile;
    if (auto found = entries_.find(key); found != entries_.end()) {
        ++hits_;
        recency_.splice(recency_.begin(), recency_, found->second.recency);
        return found->second.tile;
    }
    while (used_bytes_ > max_bytes_ - charge) {
        const auto oldest = std::prev(recency_.end());
        used_bytes_ -= entries_.at(*oldest).charged_bytes;
        entries_.erase(*oldest);
        recency_.erase(oldest);
    }
    recency_.push_front(key);
    try {
        entries_.emplace(key, Entry{tile, recency_.begin(), charge});
    } catch (...) {
        recency_.pop_front();
        throw;
    }
    used_bytes_ += charge;
    return tile;
}

TileCache::Stats TileCache::stats() const {
    std::lock_guard lock(mutex_);
    return {entries_.size(), used_bytes_, hits_, misses_};
}

void TileCache::clear() {
    std::lock_guard lock(mutex_);
    ++generation_;
    entries_.clear(); recency_.clear(); used_bytes_ = 0;
    hits_ = 0; misses_ = 0;
}

namespace {

constexpr std::size_t max_manifest_bytes = 16 * 1024 * 1024;
constexpr int max_json_depth = 64;

bool valid_utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i]);
        if (first < 0x80) { ++i; continue; }
        int following = 0;
        std::uint32_t code = 0, minimum = 0;
        if ((first & 0xe0) == 0xc0) { following = 1; code = first & 0x1f; minimum = 0x80; }
        else if ((first & 0xf0) == 0xe0) { following = 2; code = first & 0x0f; minimum = 0x800; }
        else if ((first & 0xf8) == 0xf0) { following = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (i + following >= text.size()) return false;
        for (int j = 1; j <= following; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
            return false;
        i += static_cast<std::size_t>(following) + 1;
    }
    return true;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) out.push_back(static_cast<char>(code));
    else if (code < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {
        if (input.size() > max_manifest_bytes || !valid_utf8(input))
            throw std::invalid_argument("edit JSON exceeds limit or is not UTF-8");
    }
    EditValue parse() {
        auto value = parse_value(0);
        whitespace();
        if (position_ != input_.size()) fail();
        return value;
    }
private:
    [[noreturn]] void fail() const { throw std::invalid_argument("invalid edit JSON"); }
    void whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) ++position_;
    }
    bool consume(char c) {
        whitespace();
        if (position_ < input_.size() && input_[position_] == c) {
            ++position_; return true;
        }
        return false;
    }
    void expect(char c) { if (!consume(c)) fail(); }
    std::uint32_t hex4() {
        if (input_.size() - position_ < 4) fail();
        std::uint32_t code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9') code |= c - '0';
            else if (c >= 'a' && c <= 'f') code |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') code |= c - 'A' + 10;
            else fail();
        }
        return code;
    }
    std::string parse_string() {
        expect('"');
        std::string out;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail();
            if (c != '\\') { out.push_back(c); continue; }
            if (position_ == input_.size()) fail();
            switch (input_[position_++]) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                auto code = hex4();
                if (code >= 0xd800 && code <= 0xdbff) {
                    if (input_.size() - position_ < 2 ||
                        input_[position_++] != '\\' || input_[position_++] != 'u') fail();
                    const auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail();
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                } else if (code >= 0xdc00 && code <= 0xdfff) fail();
                append_utf8(out, code);
                break;
            }
            default: fail();
            }
        }
        fail();
    }
    EditValue parse_number() {
        const auto start = position_;
        if (input_[position_] == '-') ++position_;
        if (position_ == input_.size()) fail();
        if (input_[position_] == '0') ++position_;
        else {
            if (input_[position_] < '1' || input_[position_] > '9') fail();
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        bool floating = false;
        if (position_ < input_.size() && input_[position_] == '.') {
            floating = true; ++position_;
            if (position_ == input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') fail();
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            floating = true; ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ == input_.size() || input_[position_] < '0' ||
                input_[position_] > '9') fail();
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        const auto token = input_.substr(start, position_ - start);
        if (!floating) {
            std::int64_t integer{};
            const auto result = std::from_chars(token.data(), token.data() + token.size(), integer);
            if (result.ec == std::errc{} && result.ptr == token.data() + token.size())
                return EditValue{integer};
            fail();
        }
        double number{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), number);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size() ||
            !std::isfinite(number)) fail();
        return EditValue{number};
    }
    EditValue parse_value(int depth) {
        if (depth > max_json_depth) fail();
        whitespace();
        if (position_ == input_.size()) fail();
        if (consume('{')) {
            EditValue::Object object;
            if (consume('}')) return EditValue{std::move(object)};
            do {
                whitespace();
                if (position_ == input_.size() || input_[position_] != '"') fail();
                auto key = parse_string();
                expect(':');
                if (!object.emplace(std::move(key), parse_value(depth + 1)).second) fail();
                if (consume('}')) return EditValue{std::move(object)};
                expect(',');
            } while (true);
        }
        if (consume('[')) {
            EditValue::Array array;
            if (consume(']')) return EditValue{std::move(array)};
            do {
                array.push_back(parse_value(depth + 1));
                if (consume(']')) return EditValue{std::move(array)};
                expect(',');
            } while (true);
        }
        if (input_[position_] == '"') return EditValue{parse_string()};
        for (const auto [word, value] : {
                 std::pair<std::string_view, EditValue>{"true", EditValue{true}},
                 {"false", EditValue{false}}, {"null", EditValue{nullptr}}}) {
            if (input_.substr(position_, word.size()) == word) {
                position_ += word.size(); return value;
            }
        }
        if (input_[position_] == '-' ||
            (input_[position_] >= '0' && input_[position_] <= '9')) return parse_number();
        fail();
    }
    std::string_view input_;
    std::size_t position_ = 0;
};

void append_string(std::string& out, std::string_view value) {
    if (!valid_utf8(value)) throw std::invalid_argument("edit string is not UTF-8");
    constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
        else if (c < 0x20) {
            out += "\\u00";
            out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]);
        } else out.push_back(static_cast<char>(c));
    }
    out.push_back('"');
}

void append_json(std::string& out, const EditValue& value, int depth = 0) {
    if (depth > max_json_depth) throw std::invalid_argument("edit value exceeds depth limit");
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) out += "null";
        else if constexpr (std::is_same_v<T, bool>) out += item ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::int64_t>) out += std::to_string(item);
        else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(item)) throw std::invalid_argument("non-finite edit parameter");
            if (item == 0.0) { out += "0.0"; return; }
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), item,
                                               std::chars_format::general,
                                               std::numeric_limits<double>::max_digits10);
            if (result.ec != std::errc{}) throw std::invalid_argument("invalid edit number");
            const std::string_view number(buffer, result.ptr - buffer);
            out.append(number);
            if (number.find_first_of(".eE") == std::string_view::npos) out += ".0";
        } else if constexpr (std::is_same_v<T, std::string>) append_string(out, item);
        else if constexpr (std::is_same_v<T, EditValue::Array>) {
            out.push_back('[');
            for (std::size_t i = 0; i < item.size(); ++i) {
                if (i) out.push_back(',');
                append_json(out, item[i], depth + 1);
            }
            out.push_back(']');
        } else {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, child] : item) {
                if (!first) out.push_back(','); first = false;
                append_string(out, key); out.push_back(':');
                append_json(out, child, depth + 1);
            }
            out.push_back('}');
        }
    }, value.data);
}

EditValue::Object object(const EditValue& value) {
    if (auto* result = std::get_if<EditValue::Object>(&value.data)) return *result;
    throw std::invalid_argument("edit JSON object expected");
}
EditValue::Array array(const EditValue& value) {
    if (auto* result = std::get_if<EditValue::Array>(&value.data)) return *result;
    throw std::invalid_argument("edit JSON array expected");
}
EditValue take(EditValue::Object& object, const char* key) {
    auto found = object.find(key);
    if (found == object.end()) throw std::invalid_argument(std::string("missing edit field: ") + key);
    auto value = std::move(found->second);
    object.erase(found);
    return value;
}
std::string string(EditValue value) {
    if (auto* result = std::get_if<std::string>(&value.data)) return std::move(*result);
    throw std::invalid_argument("edit string expected");
}
std::uint32_t positive_u32(EditValue value) {
    const auto* integer = std::get_if<std::int64_t>(&value.data);
    if (!integer || *integer <= 0 ||
        *integer > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("positive edit version expected");
    return static_cast<std::uint32_t>(*integer);
}
bool boolean(EditValue value) {
    if (auto* result = std::get_if<bool>(&value.data)) return *result;
    throw std::invalid_argument("edit boolean expected");
}
double number(EditValue value) {
    if (auto* result = std::get_if<double>(&value.data)) return *result;
    if (auto* result = std::get_if<std::int64_t>(&value.data)) return static_cast<double>(*result);
    throw std::invalid_argument("edit number expected");
}
std::map<std::string, std::string> edges(EditValue value) {
    std::map<std::string, std::string> result;
    for (auto& [name, target] : object(value)) result.emplace(name, string(std::move(target)));
    return result;
}
EditValue::Object edge_object(const std::map<std::string, std::string>& edges) {
    EditValue::Object result;
    for (const auto& [name, target] : edges) result.emplace(name, EditValue{target});
    return result;
}

std::string_view working_name(WorkingSpace space) {
    switch (space) {
    case WorkingSpace::LinearProPhotoD50: return "linear_prophoto_d50";
    case WorkingSpace::LinearRec2020D65: return "linear_rec2020_d65";
    }
    throw std::invalid_argument("unknown working space");
}
WorkingSpace parse_working(std::string_view name) {
    if (name == "linear_prophoto_d50") return WorkingSpace::LinearProPhotoD50;
    if (name == "linear_rec2020_d65") return WorkingSpace::LinearRec2020D65;
    throw std::invalid_argument("unknown working space");
}
std::string_view source_name(EditSourceKind kind) {
    switch (kind) {
    case EditSourceKind::DecodedBayerU16: return "decoded_bayer_u16";
    case EditSourceKind::SceneLinearRasterF32: return "scene_linear_raster_f32";
    case EditSourceKind::IccRasterU16: return "icc_raster_u16";
    }
    throw std::invalid_argument("unknown source kind");
}
EditSourceKind parse_source(std::string_view name) {
    if (name == "decoded_bayer_u16") return EditSourceKind::DecodedBayerU16;
    if (name == "scene_linear_raster_f32") return EditSourceKind::SceneLinearRasterF32;
    if (name == "icc_raster_u16") return EditSourceKind::IccRasterU16;
    throw std::invalid_argument("unknown source kind");
}
std::string_view domain_name(EditDomain domain) {
    switch (domain) {
    case EditDomain::CameraLinear: return "camera_linear";
    case EditDomain::SceneLinearProPhotoD50: return "scene_linear_prophoto_d50";
    case EditDomain::SceneLinearRec2020D65: return "scene_linear_rec2020_d65";
    case EditDomain::SceneLinearSrgb: return "scene_linear_srgb";
    case EditDomain::ToneMappedUnmanaged: return "tone_mapped_unmanaged";
    case EditDomain::DisplayLinearSrgb: return "display_linear_srgb";
    case EditDomain::DisplayEncodedSrgb: return "display_encoded_srgb";
    case EditDomain::DisplayEncodedIcc: return "display_encoded_icc";
    case EditDomain::UnmanagedBounded: return "unmanaged_bounded";
    }
    throw std::invalid_argument("unknown edit domain");
}
EditDomain parse_domain(std::string_view name) {
    for (auto domain : {EditDomain::CameraLinear, EditDomain::SceneLinearProPhotoD50,
                        EditDomain::SceneLinearRec2020D65, EditDomain::SceneLinearSrgb,
                        EditDomain::ToneMappedUnmanaged, EditDomain::DisplayLinearSrgb,
                        EditDomain::DisplayEncodedSrgb, EditDomain::DisplayEncodedIcc,
                        EditDomain::UnmanagedBounded})
        if (domain_name(domain) == name) return domain;
    throw std::invalid_argument("unknown edit domain");
}

std::string hex_digest(const std::array<std::uint8_t, 32>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
    return result;
}
std::array<std::uint8_t, 32> parse_digest(std::string_view hex) {
    if (hex.size() != 64) throw std::invalid_argument("invalid SHA-256 digest");
    std::array<std::uint8_t, 32> digest{};
    for (std::size_t i = 0; i < 64; ++i) {
        const char c = hex[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            throw std::invalid_argument("SHA-256 digest must be lowercase hex");
        const auto nibble = static_cast<std::uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        digest[i / 2] |= static_cast<std::uint8_t>(nibble << (i % 2 ? 0 : 4));
    }
    return digest;
}
bool nonzero(const std::array<std::uint8_t, 32>& digest) {
    return std::any_of(digest.begin(), digest.end(), [](auto byte) { return byte != 0; });
}
bool valid_uuid(std::string_view id) {
    if (id.size() != 36) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-') return false;
        } else if (!((id[i] >= '0' && id[i] <= '9') ||
                     (id[i] >= 'a' && id[i] <= 'f'))) return false;
    }
    return true;
}
bool valid_name(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
    });
}
void validate_icc(const IccProfileIdentity& icc) {
    if (!nonzero(icc.profile_sha256) ||
        (icc.intent != "perceptual" && icc.intent != "relative_colorimetric" &&
         icc.intent != "saturation" && icc.intent != "absolute_colorimetric") ||
        !valid_name(icc.engine) || !valid_name(icc.engine_version))
        throw std::invalid_argument("invalid ICC edit identity");
}
EditValue::Object icc_object(const IccProfileIdentity& icc) {
    return {{"black_point_compensation", EditValue{icc.black_point_compensation}},
            {"engine", EditValue{icc.engine}},
            {"engine_version", EditValue{icc.engine_version}},
            {"intent", EditValue{icc.intent}},
            {"profile_sha256", EditValue{hex_digest(icc.profile_sha256)}}};
}
IccProfileIdentity parse_icc(EditValue value) {
    auto fields = object(value);
    IccProfileIdentity icc;
    icc.profile_sha256 = parse_digest(string(take(fields, "profile_sha256")));
    icc.intent = string(take(fields, "intent"));
    icc.black_point_compensation = boolean(take(fields, "black_point_compensation"));
    icc.engine = string(take(fields, "engine"));
    icc.engine_version = string(take(fields, "engine_version"));
    if (!fields.empty()) throw std::invalid_argument("unknown ICC policy field");
    validate_icc(icc);
    return icc;
}

EditValue::Object source_object(const EditSource& source) {
    EditValue::Object result{{"content_sha256", EditValue{hex_digest(source.content_sha256)}},
                             {"id", EditValue{source.id}},
                             {"kind", EditValue{std::string(source_name(source.kind))}}};
    if (source.working_space)
        result.emplace("working_space", EditValue{std::string(working_name(*source.working_space))});
    if (source.icc_input) result.emplace("icc_input", EditValue{icc_object(*source.icc_input)});
    return result;
}
EditSource parse_edit_source(EditValue value) {
    auto fields = object(value);
    EditSource source;
    source.id = string(take(fields, "id"));
    source.kind = parse_source(string(take(fields, "kind")));
    source.content_sha256 = parse_digest(string(take(fields, "content_sha256")));
    if (auto found = fields.find("working_space"); found != fields.end()) {
        source.working_space = parse_working(string(std::move(found->second)));
        fields.erase(found);
    }
    if (auto found = fields.find("icc_input"); found != fields.end()) {
        source.icc_input = parse_icc(std::move(found->second)); fields.erase(found);
    }
    if (!fields.empty()) throw std::invalid_argument("unknown edit source field");
    return source;
}

EditValue::Object operation_object(const EditOperation& op) {
    EditValue::Object result = op.extra_fields;
    const EditValue::Object known{
        {"blend_mode", EditValue{op.blend_mode}},
        {"enabled", EditValue{op.enabled}},
        {"id", EditValue{op.id}},
        {"input_domain", EditValue{std::string(domain_name(op.input_domain))}},
        {"inputs", EditValue{edge_object(op.inputs)}},
        {"masks", EditValue{edge_object(op.masks)}},
        {"opacity", EditValue{op.opacity}},
        {"output_domain", EditValue{std::string(domain_name(op.output_domain))}},
        {"parameters", EditValue{op.parameters}},
        {"processing_version", EditValue{static_cast<std::int64_t>(op.processing_version)}},
        {"schema_version", EditValue{static_cast<std::int64_t>(op.schema_version)}},
        {"type", EditValue{op.type_id}}};
    for (const auto& [key, value] : known)
        if (!result.emplace(key, value).second)
            throw std::invalid_argument("operation extension collides with known field");
    return result;
}
EditOperation parse_operation(EditValue value) {
    auto fields = object(value);
    EditOperation op;
    op.id = string(take(fields, "id"));
    op.type_id = string(take(fields, "type"));
    op.schema_version = positive_u32(take(fields, "schema_version"));
    op.processing_version = positive_u32(take(fields, "processing_version"));
    op.enabled = boolean(take(fields, "enabled"));
    op.input_domain = parse_domain(string(take(fields, "input_domain")));
    op.output_domain = parse_domain(string(take(fields, "output_domain")));
    op.parameters = object(take(fields, "parameters"));
    op.inputs = edges(take(fields, "inputs"));
    op.masks = edges(take(fields, "masks"));
    op.blend_mode = string(take(fields, "blend_mode"));
    op.opacity = number(take(fields, "opacity"));
    op.extra_fields = std::move(fields);
    return op;
}

void validate_known_parameters(const EditOperation& op) {
    auto scalar = [&](const char* name) {
        auto found = op.parameters.find(name);
        if (found == op.parameters.end()) throw std::invalid_argument("missing operation parameter");
        return number(found->second);
    };
    if (op.type_id == "rawengine.exposure") {
        if (op.schema_version != 1 || op.parameters.size() != 1 ||
            std::abs(scalar("stops")) > 32.0)
            throw std::invalid_argument("invalid exposure operation");
    } else if (op.type_id == "rawengine.white_balance") {
        if (op.schema_version != 1 || op.parameters.size() != 3)
            throw std::invalid_argument("invalid white-balance operation");
        for (auto name : {"red_gain", "green_gain", "blue_gain"}) {
            const auto gain = scalar(name);
            if (gain <= 0.0 || gain > 65536.0)
                throw std::invalid_argument("invalid white-balance gain");
        }
    } else if (op.type_id == "rawengine.tone_curve") {
        if (op.schema_version != 1 || op.parameters.size() != 2 ||
            scalar("shoulder") <= 0.0 || scalar("gamma") <= 0.0)
            throw std::invalid_argument("invalid tone-curve operation");
    } else if (op.type_id == "rawengine.box_blur") {
        if (op.schema_version != 1 || op.parameters.size() != 1 ||
            !op.parameters.contains("radius") ||
            positive_u32(op.parameters.at("radius")) > 8)
            throw std::invalid_argument("invalid box-blur operation");
    } else if (op.type_id == "rawengine.camera_to_working") {
        if (op.schema_version != 1 || op.parameters.size() != 1 ||
            !op.parameters.contains("matrix"))
            throw std::invalid_argument("invalid camera-to-working operation");
        const auto values = array(op.parameters.at("matrix"));
        if (values.size() != 9)
            throw std::invalid_argument("camera matrix needs nine values");
        for (const auto& value : values) (void)number(value);
    } else if (op.type_id == "rawengine.working_space_convert" ||
               op.type_id == "rawengine.working_to_srgb" ||
               op.type_id == "rawengine.output_clip" ||
               op.type_id == "rawengine.srgb_encode" ||
               op.type_id == "rawengine.icc_display") {
        if (op.schema_version != 1 || !op.parameters.empty())
            throw std::invalid_argument("invalid parameterless edit operation");
    } else if (op.type_id == "rawengine.legacy.fixed_chain") {
        if (op.schema_version != 1 || op.parameters.size() != 1 ||
            op.parameters.find("recipe") == op.parameters.end() ||
            !std::holds_alternative<EditValue::Object>(op.parameters.at("recipe").data))
            throw std::invalid_argument("invalid legacy recipe snapshot");
    }
}

} // namespace

void validate_edit_manifest(const EditManifest& manifest) {
    if (manifest.format_version != 2 || manifest.processing_version == 0 ||
        (manifest.working_space != WorkingSpace::LinearProPhotoD50 &&
         manifest.working_space != WorkingSpace::LinearRec2020D65) ||
        manifest.sources.empty() || manifest.sources.size() > 100000 ||
        manifest.operations.size() > 100000)
        throw std::invalid_argument("unsupported or incomplete edit manifest");
    std::set<std::string> ids;
    for (const auto& source : manifest.sources) {
        if (!valid_uuid(source.id) || !ids.insert(source.id).second ||
            !nonzero(source.content_sha256))
            throw std::invalid_argument("invalid or duplicate edit source identity");
        source_name(source.kind);
        if (source.icc_input) validate_icc(*source.icc_input);
        if ((source.kind == EditSourceKind::IccRasterU16) != source.icc_input.has_value())
            throw std::invalid_argument("ICC raster source requires input profile identity");
        if ((source.kind == EditSourceKind::DecodedBayerU16) == source.working_space.has_value())
            throw std::invalid_argument("raster source needs a working space; Bayer source cannot have one");
        if (source.working_space) working_name(*source.working_space);
    }
    std::map<std::string, const EditOperation*> operations;
    for (const auto& op : manifest.operations) {
        if (!valid_uuid(op.id) || !ids.insert(op.id).second ||
            !valid_name(op.type_id) || !op.schema_version || !op.processing_version ||
            op.processing_version > manifest.processing_version ||
            !valid_name(op.blend_mode) || !std::isfinite(op.opacity) ||
            op.opacity < 0.0 || op.opacity > 1.0)
            throw std::invalid_argument("invalid or duplicate edit operation");
        domain_name(op.input_domain); domain_name(op.output_domain);
        for (const auto& [name, target] : op.inputs)
            if (!valid_name(name) || !valid_uuid(target))
                throw std::invalid_argument("invalid edit input edge");
        for (const auto& [name, target] : op.masks)
            if (!valid_name(name) || !valid_uuid(target))
                throw std::invalid_argument("invalid edit mask edge");
        validate_known_parameters(op);
        operations.emplace(op.id, &op);
    }
    if (!ids.contains(manifest.output_id))
        throw std::invalid_argument("edit output does not exist");
    if (manifest.output_profile) validate_icc(*manifest.output_profile);
    const auto output = operations.find(manifest.output_id);
    const bool icc_output = output != operations.end() &&
                            output->second->output_domain == EditDomain::DisplayEncodedIcc;
    if (icc_output != manifest.output_profile.has_value())
        throw std::invalid_argument("ICC output domain and profile identity must agree");
    for (const auto& op : manifest.operations) {
        for (const auto* edges : {&op.inputs, &op.masks})
            for (const auto& [name, target] : *edges)
                if (!ids.contains(target))
                    throw std::invalid_argument("edit edge target does not exist");
    }
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& op : manifest.operations) indegree.emplace(op.id, 0);
    for (const auto& op : manifest.operations)
        for (const auto* edges : {&op.inputs, &op.masks})
            for (const auto& [name, target] : *edges)
                if (operations.contains(target)) {
                    ++indegree.at(op.id);
                    dependents[target].push_back(op.id);
                }
    std::queue<std::string> ready;
    for (const auto& [id, count] : indegree) if (!count) ready.push(id);
    std::size_t visited = 0;
    while (!ready.empty()) {
        auto id = std::move(ready.front()); ready.pop(); ++visited;
        for (const auto& dependent : dependents[id])
            if (--indegree.at(dependent) == 0) ready.push(dependent);
    }
    if (visited != manifest.operations.size())
        throw std::invalid_argument("edit graph contains a cycle");
    // Also validate arbitrary values and UTF-8 strings before writing or use.
    std::string encoded;
    append_json(encoded, EditValue{manifest.extra_fields});
    for (const auto& op : manifest.operations) {
        append_json(encoded, EditValue{op.parameters});
        append_json(encoded, EditValue{op.extra_fields});
    }
}

std::string serialize_edit_manifest(const EditManifest& manifest) {
    validate_edit_manifest(manifest);
    EditValue::Array sources, operations;
    auto sorted_sources = manifest.sources;
    auto sorted_operations = manifest.operations;
    std::sort(sorted_sources.begin(), sorted_sources.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    std::sort(sorted_operations.begin(), sorted_operations.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    for (const auto& source : sorted_sources) sources.emplace_back(source_object(source));
    for (const auto& op : sorted_operations) operations.emplace_back(operation_object(op));
    EditValue::Object root = manifest.extra_fields;
    const EditValue::Object known{
        {"format_version", EditValue{static_cast<std::int64_t>(manifest.format_version)}},
        {"operations", EditValue{std::move(operations)}},
        {"output", EditValue{manifest.output_id}},
        {"processing_version", EditValue{static_cast<std::int64_t>(manifest.processing_version)}},
        {"sources", EditValue{std::move(sources)}},
        {"working_space", EditValue{std::string(working_name(manifest.working_space))}}};
    for (const auto& [key, value] : known)
        if (!root.emplace(key, value).second)
            throw std::invalid_argument("manifest extension collides with known field");
    if (manifest.output_profile &&
        !root.emplace("output_profile", EditValue{icc_object(*manifest.output_profile)}).second)
        throw std::invalid_argument("manifest output profile collision");
    std::string encoded;
    append_json(encoded, EditValue{std::move(root)});
    if (encoded.size() > max_manifest_bytes)
        throw std::length_error("serialized edit manifest exceeds limit");
    return encoded;
}

EditManifest parse_edit_manifest(std::string_view json) {
    auto root = object(JsonParser(json).parse());
    EditManifest manifest;
    const auto saved_format = positive_u32(take(root, "format_version"));
    if (saved_format != 1 && saved_format != 2)
        throw std::invalid_argument("unsupported edit manifest format version");
    manifest.format_version = 2;
    manifest.processing_version = positive_u32(take(root, "processing_version"));
    manifest.working_space = parse_working(string(take(root, "working_space")));
    manifest.output_id = string(take(root, "output"));
    for (auto& item : array(take(root, "sources")))
        manifest.sources.push_back(parse_edit_source(std::move(item)));
    if (saved_format == 1)
        for (auto& source : manifest.sources)
            if (source.kind != EditSourceKind::DecodedBayerU16 && !source.working_space)
                source.working_space = manifest.working_space;
    for (auto& item : array(take(root, "operations")))
        manifest.operations.push_back(parse_operation(std::move(item)));
    if (auto found = root.find("output_profile"); found != root.end()) {
        manifest.output_profile = parse_icc(std::move(found->second)); root.erase(found);
    }
    manifest.extra_fields = std::move(root);
    validate_edit_manifest(manifest);
    return manifest;
}

EditManifest snapshot_legacy_recipe(EditSource source, GraphRecipe recipe,
                                    LegacyRecipeEra era, std::string operation_id,
                                    std::optional<IccProfileIdentity> output_profile) {
    EditManifest manifest;
    switch (era) {
    case LegacyRecipeEra::ImplicitRec2020:
        manifest.processing_version = kLegacyRec2020ProcessingVersion;
        manifest.working_space = WorkingSpace::LinearRec2020D65;
        break;
    case LegacyRecipeEra::ImplicitProPhoto:
        manifest.processing_version = kCurrentEditProcessingVersion;
        manifest.working_space = WorkingSpace::LinearProPhotoD50;
        break;
    default: throw std::invalid_argument("unknown legacy recipe provenance");
    }
    if (recipe.output_mode == OutputMode::IccDisplay && !output_profile)
        throw std::invalid_argument("legacy ICC recipe needs exact output profile identity");
    if (recipe.output_mode != OutputMode::IccDisplay && output_profile)
        throw std::invalid_argument("unexpected legacy output profile identity");
    if (recipe.camera_color && recipe.camera_color->target != manifest.working_space)
        throw std::invalid_argument("legacy camera target conflicts with declared provenance");
    if (source.kind != EditSourceKind::DecodedBayerU16 &&
        (recipe.red_gain != 1.0f || recipe.green_gain != 1.0f ||
         recipe.blue_gain != 1.0f || recipe.camera_color))
        throw std::invalid_argument("legacy raster recipe has RAW calibration controls");
    if (source.working_space && *source.working_space != manifest.working_space)
        throw std::invalid_argument("legacy raster source space conflicts with declared provenance");
    if (source.kind == EditSourceKind::DecodedBayerU16 && !recipe.camera_color &&
        recipe.output_mode != OutputMode::LegacyBounded)
        throw std::invalid_argument("legacy RAW preview requires camera color calibration");
    manifest.sources.push_back(std::move(source));
    manifest.output_profile = std::move(output_profile);
    EditOperation op;
    op.id = std::move(operation_id);
    op.type_id = "rawengine.legacy.fixed_chain";
    op.processing_version = manifest.processing_version;
    op.input_domain = manifest.sources.front().kind == EditSourceKind::DecodedBayerU16
                          ? EditDomain::CameraLinear
                          : manifest.working_space == WorkingSpace::LinearProPhotoD50
                                ? EditDomain::SceneLinearProPhotoD50
                                : EditDomain::SceneLinearRec2020D65;
    switch (recipe.output_mode) {
    case OutputMode::LegacyBounded: op.output_domain = EditDomain::UnmanagedBounded; break;
    case OutputMode::SrgbPreview: op.output_domain = EditDomain::DisplayEncodedSrgb; break;
    case OutputMode::IccDisplay: op.output_domain = EditDomain::DisplayEncodedIcc; break;
    default: throw std::invalid_argument("unknown legacy output mode");
    }
    EditValue::Object recipe_fields{
        {"red_gain", EditValue{static_cast<double>(recipe.red_gain)}},
        {"green_gain", EditValue{static_cast<double>(recipe.green_gain)}},
        {"blue_gain", EditValue{static_cast<double>(recipe.blue_gain)}},
        {"exposure_stops", EditValue{static_cast<double>(recipe.exposure_stops)}},
        {"tone_shoulder", EditValue{static_cast<double>(recipe.tone_shoulder)}},
        {"tone_gamma", EditValue{static_cast<double>(recipe.tone_gamma)}},
        {"output_mode", EditValue{recipe.output_mode == OutputMode::LegacyBounded
                                       ? std::string("legacy_bounded")
                                       : recipe.output_mode == OutputMode::SrgbPreview
                                             ? std::string("srgb_preview")
                                             : std::string("icc_display")}}};
    if (recipe.camera_color) {
        EditValue::Array matrix;
        for (double coefficient : recipe.camera_color->camera_to_xyz_d50)
            matrix.emplace_back(coefficient);
        recipe_fields.emplace("camera_to_xyz_d50", EditValue{std::move(matrix)});
        recipe_fields.emplace("camera_target", EditValue{
            std::string(working_name(manifest.working_space))});
    }
    op.parameters.emplace("recipe", EditValue{std::move(recipe_fields)});
    op.inputs.emplace("image", manifest.sources.front().id);
    manifest.output_id = op.id;
    manifest.operations.push_back(std::move(op));
    validate_edit_manifest(manifest);
    return manifest;
}

namespace {

EditDomain descriptor_domain(const ImageDescriptor& descriptor) {
    if (descriptor == ImageDescriptor::camera_linear()) return EditDomain::CameraLinear;
    if (descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearProPhotoD50))
        return EditDomain::SceneLinearProPhotoD50;
    if (descriptor == ImageDescriptor::scene_linear(WorkingSpace::LinearRec2020D65))
        return EditDomain::SceneLinearRec2020D65;
    if (descriptor == ImageDescriptor::linear_srgb()) return EditDomain::SceneLinearSrgb;
    if (descriptor.domain == PixelDomain::ToneMappedUnmanagedRGB &&
        descriptor.transfer == TransferFunction::CustomTone)
        return EditDomain::ToneMappedUnmanaged;
    if (descriptor == ImageDescriptor::display_linear_srgb())
        return EditDomain::DisplayLinearSrgb;
    if (descriptor == ImageDescriptor::srgb_output()) return EditDomain::DisplayEncodedSrgb;
    if (descriptor.domain == PixelDomain::DisplayEncodedRGB &&
        descriptor.primaries == ColorPrimaries::ICCProfile &&
        descriptor.transfer == TransferFunction::ICCProfile)
        return EditDomain::DisplayEncodedIcc;
    if (descriptor.domain == PixelDomain::BoundedUnmanagedRGB &&
        descriptor.transfer == TransferFunction::CustomTone)
        return EditDomain::UnmanagedBounded;
    throw std::invalid_argument("runtime node has an unsupported edit color domain");
}

bool supported_operation(std::string_view type) {
    for (auto known : {"rawengine.white_balance", "rawengine.exposure",
                       "rawengine.camera_to_working", "rawengine.working_space_convert",
                       "rawengine.working_to_srgb", "rawengine.tone_curve",
                       "rawengine.box_blur",
                       "rawengine.output_clip", "rawengine.srgb_encode",
                       "rawengine.icc_display", "rawengine.legacy.fixed_chain"})
        if (type == known) return true;
    return false;
}

WorkingSpace working_from_domain(EditDomain domain) {
    if (domain == EditDomain::SceneLinearProPhotoD50)
        return WorkingSpace::LinearProPhotoD50;
    if (domain == EditDomain::SceneLinearRec2020D65)
        return WorkingSpace::LinearRec2020D65;
    throw std::invalid_argument("operation target is not a supported working space");
}

float scalar(const EditValue::Object& parameters, const char* key) {
    const auto found = parameters.find(key);
    if (found == parameters.end()) throw std::invalid_argument("missing operation scalar");
    const double value = number(found->second);
    if (!std::isfinite(value) ||
        std::abs(value) > std::numeric_limits<float>::max())
        throw std::invalid_argument("operation scalar exceeds float32 range");
    return static_cast<float>(value);
}

CameraColorTransform camera_transform(const EditValue::Object& parameters,
                                      WorkingSpace target, const char* key) {
    const auto found = parameters.find(key);
    if (found == parameters.end()) throw std::invalid_argument("missing camera color matrix");
    const auto values = array(found->second);
    if (values.size() != 9) throw std::invalid_argument("camera matrix needs nine values");
    CameraColorTransform transform;
    transform.target = target;
    for (std::size_t i = 0; i < 9; ++i) transform.camera_to_xyz_d50[i] = number(values[i]);
    return transform;
}

void validate_single_input_metadata(const EditOperation& op) {
    if (op.inputs.size() != 1 || !op.inputs.contains("image") ||
        !op.masks.empty() || op.blend_mode != "normal" || op.opacity != 1.0 ||
        !op.extra_fields.empty())
        throw std::invalid_argument("operation uses an unsupported input, mask, blend or extension");
}

std::shared_ptr<const Node> build_legacy_chain(
    const EditOperation& op, std::shared_ptr<const Node> source,
    WorkingSpace working_space,
    const std::shared_ptr<const IccDisplayTransform>& display_transform) {
    auto fields = object(op.parameters.at("recipe"));
    const float red = scalar(fields, "red_gain");
    const float green = scalar(fields, "green_gain");
    const float blue = scalar(fields, "blue_gain");
    const float exposure = scalar(fields, "exposure_stops");
    const float shoulder = scalar(fields, "tone_shoulder");
    const float gamma = scalar(fields, "tone_gamma");
    const auto mode = string(take(fields, "output_mode"));
    std::shared_ptr<const Node> linear = source;
    if (descriptor_domain(source->output_descriptor()) == EditDomain::CameraLinear) {
        linear = std::make_shared<WhiteBalanceNode>(linear, red, green, blue);
        linear = std::make_shared<ExposureNode>(linear, exposure);
        if (fields.contains("camera_to_xyz_d50")) {
            if (string(take(fields, "camera_target")) != working_name(working_space))
                throw std::invalid_argument("legacy camera target disagrees with working space");
            linear = std::make_shared<CameraToWorkingNode>(
                linear, camera_transform(fields, working_space, "camera_to_xyz_d50"));
            fields.erase("camera_to_xyz_d50");
        }
    } else {
        if (red != 1.0f || green != 1.0f || blue != 1.0f ||
            fields.contains("camera_to_xyz_d50") || fields.contains("camera_target"))
            throw std::invalid_argument("legacy raster recipe contains RAW controls");
        linear = std::make_shared<ExposureNode>(linear, exposure);
    }
    if (mode == "srgb_preview" || mode == "icc_display")
        linear = std::make_shared<WorkingToSrgbNode>(linear);
    else if (mode != "legacy_bounded")
        throw std::invalid_argument("unknown legacy output mode");
    if (fields.size() != 6)
        throw std::invalid_argument("legacy recipe contains unrecognized fields");
    auto tone = std::make_shared<ToneCurveNode>(linear, shoulder, gamma);
    if (mode == "srgb_preview") return std::make_shared<SrgbEncodeNode>(tone);
    if (mode == "icc_display")
        return std::make_shared<IccDisplayNode>(tone, display_transform);
    return std::make_shared<OutputClipNode>(tone);
}

std::shared_ptr<const Node> build_operation(
    const EditOperation& op, std::shared_ptr<const Node> input,
    WorkingSpace working_space, Rect source_bounds,
    const std::shared_ptr<const IccDisplayTransform>& display_transform) {
    const auto& p = op.parameters;
    if (op.type_id == "rawengine.white_balance")
        return std::make_shared<WhiteBalanceNode>(input, scalar(p, "red_gain"),
                                                  scalar(p, "green_gain"),
                                                  scalar(p, "blue_gain"));
    if (op.type_id == "rawengine.exposure")
        return std::make_shared<ExposureNode>(input, scalar(p, "stops"));
    if (op.type_id == "rawengine.camera_to_working")
        return std::make_shared<CameraToWorkingNode>(
            input, camera_transform(p, working_from_domain(op.output_domain), "matrix"));
    if (op.type_id == "rawengine.working_space_convert")
        return std::make_shared<WorkingSpaceConvertNode>(
            input, working_from_domain(op.output_domain));
    if (op.type_id == "rawengine.working_to_srgb")
        return std::make_shared<WorkingToSrgbNode>(input);
    if (op.type_id == "rawengine.tone_curve")
        return std::make_shared<ToneCurveNode>(input, scalar(p, "shoulder"),
                                               scalar(p, "gamma"));
    if (op.type_id == "rawengine.box_blur")
        return std::make_shared<BoxBlurNode>(
            input, source_bounds, positive_u32(p.at("radius")));
    if (op.type_id == "rawengine.output_clip")
        return std::make_shared<OutputClipNode>(input);
    if (op.type_id == "rawengine.srgb_encode")
        return std::make_shared<SrgbEncodeNode>(input);
    if (op.type_id == "rawengine.icc_display")
        return std::make_shared<IccDisplayNode>(input, display_transform);
    if (op.type_id == "rawengine.legacy.fixed_chain")
        return build_legacy_chain(op, input, working_space, display_transform);
    throw std::invalid_argument("unknown operation cannot execute: " + op.type_id);
}

std::array<std::uint8_t, 32> cache_signature(
    const EditValue::Object& identity,
    const std::optional<std::array<std::uint8_t, 32>>& upstream = std::nullopt,
    const std::optional<IccProfileIdentity>& output_profile = std::nullopt) {
    std::string encoded;
    append_json(encoded, EditValue{identity});
    Sha256 hash;
    constexpr char version[] = "librawops.tile-cache.v1";
    hash.update(version, sizeof(version));
    if (upstream) hash.update(upstream->data(), upstream->size());
    hash.update(encoded.data(), encoded.size());
    if (output_profile) {
        encoded.clear();
        append_json(encoded, EditValue{icc_object(*output_profile)});
        hash.update(encoded.data(), encoded.size());
    }
    return hash.finish();
}

class CachedNode final : public Node {
public:
    CachedNode(std::shared_ptr<const Node> node, std::shared_ptr<TileCache> cache,
               std::array<std::uint8_t, 32> signature)
        : node_(std::move(node)), cache_(std::move(cache)), signature_(signature) {}
    Tile render(Rect bounds) const override { return cache_->render(*node_, signature_, bounds); }
    ImageDescriptor output_descriptor() const noexcept override {
        return node_->output_descriptor();
    }
    Rect input_region(Rect output, Rect source_bounds) const override {
        return node_->input_region(output, source_bounds);
    }
    const Node* input_node() const noexcept override { return node_->input_node(); }
    std::optional<IccProfileIdentity> input_icc_identity() const override {
        return node_->input_icc_identity();
    }
    std::optional<std::array<std::uint8_t, 32>> source_fingerprint() const override {
        return node_->source_fingerprint();
    }
    std::optional<Rect> source_bounds() const override { return node_->source_bounds(); }
private:
    std::shared_ptr<const Node> node_;
    std::shared_ptr<TileCache> cache_;
    std::array<std::uint8_t, 32> signature_;
};

} // namespace

ExecutableEditGraph::ExecutableEditGraph(
    EditManifest manifest, std::vector<BoundEditSource> sources,
    std::shared_ptr<const IccDisplayTransform> display_transform,
    std::shared_ptr<TileCache> cache)
    : manifest_(std::move(manifest)) {
    validate_edit_manifest(manifest_);
    if (manifest_.processing_version != kLegacyRec2020ProcessingVersion &&
        manifest_.processing_version != kCurrentEditProcessingVersion)
        throw std::invalid_argument("unsupported edit processing version");
    if (manifest_.processing_version == kLegacyRec2020ProcessingVersion &&
        manifest_.working_space != WorkingSpace::LinearRec2020D65)
        throw std::invalid_argument("legacy process version requires Rec.2020");
    if (display_transform) {
        if (!manifest_.output_profile ||
            display_transform->output_icc_identity() != manifest_.output_profile ||
            display_transform->profile_sha256() != manifest_.output_profile->profile_sha256)
            throw std::invalid_argument("runtime ICC output policy differs from manifest");
    } else if (manifest_.output_profile)
        throw std::invalid_argument("ICC output transform is missing");

    struct Runtime {
        std::shared_ptr<const Node> node;
        Rect bounds;
        std::array<std::uint8_t, 32> signature;
    };
    std::map<std::string, Runtime> built;
    if (sources.size() != manifest_.sources.size())
        throw std::invalid_argument("source binding count differs from manifest");
    std::map<std::string, const BoundEditSource*> bindings;
    for (const auto& binding : sources)
        if (!bindings.emplace(binding.identity.id, &binding).second)
            throw std::invalid_argument("duplicate source binding");
    for (const auto& record : manifest_.sources) {
        const auto found = bindings.find(record.id);
        if (found == bindings.end())
            throw std::invalid_argument("source binding is missing");
        const auto& binding = *found->second;
        if (binding.identity != record || !binding.node ||
            !binding.bounds.width || !binding.bounds.height ||
            static_cast<std::uint64_t>(binding.bounds.x) + binding.bounds.width >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1 ||
            static_cast<std::uint64_t>(binding.bounds.y) + binding.bounds.height >
                static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1)
            throw std::invalid_argument("invalid source binding or bounds");
        if (binding.node->source_fingerprint() != record.content_sha256)
            throw std::invalid_argument("runtime source fingerprint differs from manifest");
        const auto actual_bounds = binding.node->source_bounds();
        if (!actual_bounds || actual_bounds->x != binding.bounds.x ||
            actual_bounds->y != binding.bounds.y ||
            actual_bounds->width != binding.bounds.width ||
            actual_bounds->height != binding.bounds.height)
            throw std::invalid_argument("runtime source bounds differ from binding");
        const auto actual = descriptor_domain(binding.node->output_descriptor());
        const auto expected = record.kind == EditSourceKind::DecodedBayerU16
                                  ? EditDomain::CameraLinear
                                  : *record.working_space == WorkingSpace::LinearProPhotoD50
                                        ? EditDomain::SceneLinearProPhotoD50
                                        : EditDomain::SceneLinearRec2020D65;
        if (actual != expected ||
            binding.node->input_icc_identity() != record.icc_input)
            throw std::invalid_argument("runtime source color identity differs from manifest");
        auto signature = cache_signature(source_object(record));
        std::shared_ptr<const Node> node = binding.node;
        if (cache) node = std::make_shared<CachedNode>(node, cache, signature);
        built.emplace(record.id, Runtime{std::move(node), binding.bounds, signature});
    }
    std::map<std::string, const EditOperation*> operations;
    std::map<std::string, std::size_t> indegree;
    std::map<std::string, std::vector<std::string>> dependents;
    for (const auto& op : manifest_.operations) {
        operations.emplace(op.id, &op);
        indegree.emplace(op.id, 0);
    }
    for (const auto& op : manifest_.operations)
        for (const auto* edges : {&op.inputs, &op.masks})
            for (const auto& [name, target] : *edges)
                if (operations.contains(target)) {
                    ++indegree.at(op.id);
                    dependents[target].push_back(op.id);
                }
    std::queue<std::string> ready;
    for (const auto& [id, count] : indegree) if (!count) ready.push(id);
    while (!ready.empty()) {
        const auto id = std::move(ready.front()); ready.pop();
        const auto& op = *operations.at(id);
        if (!supported_operation(op.type_id))
            throw std::invalid_argument("unknown operation cannot execute: " + op.type_id);
        if (op.processing_version != manifest_.processing_version || op.schema_version != 1)
            throw std::invalid_argument("operation processing/schema version is unsupported");
        validate_single_input_metadata(op);
        const auto& upstream = built.at(op.inputs.at("image"));
        if (descriptor_domain(upstream.node->output_descriptor()) != op.input_domain)
            throw std::invalid_argument("operation input domain differs from runtime edge");
        std::shared_ptr<const Node> node;
        if (op.enabled) node = build_operation(op, upstream.node,
                                               manifest_.working_space, upstream.bounds,
                                               display_transform);
        else {
            if (op.output_domain != op.input_domain)
                throw std::invalid_argument("disabled operation cannot change color domain");
            node = upstream.node;
        }
        if (descriptor_domain(node->output_descriptor()) != op.output_domain)
            throw std::invalid_argument("operation output domain differs from runtime node");
        auto signature = upstream.signature;
        if (op.enabled) {
            signature = cache_signature(operation_object(op), upstream.signature,
                                        op.output_domain == EditDomain::DisplayEncodedIcc
                                            ? manifest_.output_profile : std::nullopt);
            if (cache) node = std::make_shared<CachedNode>(node, cache, signature);
        }
        built.emplace(id, Runtime{std::move(node), upstream.bounds, signature});
        for (const auto& dependent : dependents[id])
            if (--indegree.at(dependent) == 0) ready.push(dependent);
    }
    const auto output = built.find(manifest_.output_id);
    if (output == built.end())
        throw std::invalid_argument("edit output could not be constructed");
    output_ = output->second.node;
    bounds_ = output->second.bounds;
}

Rect ExecutableEditGraph::required_source_region(Rect output) const {
    if (output.x < bounds_.x || output.y < bounds_.y ||
        static_cast<std::uint64_t>(output.x) + output.width >
            static_cast<std::uint64_t>(bounds_.x) + bounds_.width ||
        static_cast<std::uint64_t>(output.y) + output.height >
            static_cast<std::uint64_t>(bounds_.y) + bounds_.height)
        throw std::out_of_range("planned ROI is outside graph source bounds");
    Rect required = output;
    const Node* current = output_.get();
    for (std::size_t depth = 0; current; ++depth) {
        if (depth > manifest_.operations.size() * 16 + manifest_.sources.size() + 16)
            throw std::logic_error("runtime graph input chain is cyclic");
        required = current->input_region(required, bounds_);
        current = current->input_node();
    }
    return required;
}

void Renderer::render_tiles(const ExecutableEditGraph& graph, Rect viewport,
                            const TileCallback& callback, std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    render_tiles(graph.output(), graph.source_bounds(), viewport, callback, tile_size,
                 cancellation);
}

Tile Renderer::render_image(const ExecutableEditGraph& graph, Rect viewport,
                            std::uint32_t tile_size,
                            const CancellationToken* cancellation) const {
    return render_image(graph.output(), graph.source_bounds(), viewport, tile_size,
                        cancellation);
}

std::vector<float> Renderer::render_roi(const ExecutableEditGraph& graph, Rect viewport,
                                        std::uint32_t tile_size,
                                        const CancellationToken* cancellation) const {
    auto output = render_image(graph, viewport, tile_size, cancellation);
    return std::move(output.rgb);
}

} // namespace rawengine
