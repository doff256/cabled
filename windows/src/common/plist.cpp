#include "plist.h"

#include <pugixml.hpp>

#include <charconv>
#include <cstdlib>
#include <sstream>

namespace cabled::plist {

// ---------------------------------------------------------------- Value

Value Value::boolean(bool b) {
    Value v;
    v.type_ = Type::Bool;
    v.b_ = b;
    return v;
}

Value Value::integer(int64_t i) {
    Value v;
    v.type_ = Type::Integer;
    v.i_ = i;
    return v;
}

Value Value::real(double d) {
    Value v;
    v.type_ = Type::Real;
    v.d_ = d;
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.type_ = Type::String;
    v.s_ = std::move(s);
    return v;
}

Value Value::data(std::string bytes) {
    Value v;
    v.type_ = Type::Data;
    v.s_ = std::move(bytes);
    return v;
}

Value Value::date(std::string iso8601) {
    Value v;
    v.type_ = Type::Date;
    v.s_ = std::move(iso8601);
    return v;
}

Value Value::array() {
    Value v;
    v.type_ = Type::Array;
    return v;
}

Value Value::dict() {
    Value v;
    v.type_ = Type::Dict;
    return v;
}

bool Value::asBool(bool fallback) const {
    if (type_ == Type::Bool) return b_;
    if (type_ == Type::Integer) return i_ != 0;
    return fallback;
}

int64_t Value::asInt(int64_t fallback) const {
    if (type_ == Type::Integer) return i_;
    if (type_ == Type::Real) return static_cast<int64_t>(d_);
    if (type_ == Type::Bool) return b_ ? 1 : 0;
    return fallback;
}

double Value::asReal(double fallback) const {
    if (type_ == Type::Real) return d_;
    if (type_ == Type::Integer) return static_cast<double>(i_);
    return fallback;
}

Value& Value::push(Value v) {
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::set(std::string key, Value v) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            items_[i] = std::move(v);
            return items_[i];
        }
    }
    keys_.push_back(std::move(key));
    items_.push_back(std::move(v));
    return items_.back();
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Dict) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) return &items_[i];
    }
    return nullptr;
}

// ---------------------------------------------------------------- base64

namespace {
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

std::string base64Encode(std::string_view bytes) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        const uint32_t n = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += kB64[n & 63];
        i += 3;
    }
    const size_t rest = bytes.size() - i;
    if (rest == 1) {
        const uint32_t n = uint8_t(bytes[i]) << 16;
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += "==";
    } else if (rest == 2) {
        const uint32_t n = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
        out += kB64[(n >> 18) & 63];
        out += kB64[(n >> 12) & 63];
        out += kB64[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text) {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' ) break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const int v = b64Index(c);
        if (v < 0) return std::nullopt;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

// ---------------------------------------------------------------- XML out

namespace {

void escapeInto(std::string& out, std::string_view s) {
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
}

void writeValue(std::string& out, const Value& v, int depth) {
    const std::string pad(static_cast<size_t>(depth), '\t');
    switch (v.type()) {
        case Value::Type::None:
            out += pad + "<string></string>\n";
            break;
        case Value::Type::Bool:
            out += pad + (v.asBool() ? "<true/>\n" : "<false/>\n");
            break;
        case Value::Type::Integer:
            out += pad + "<integer>" + std::to_string(v.asInt()) + "</integer>\n";
            break;
        case Value::Type::Real: {
            std::ostringstream s;
            s.precision(17);
            s << v.asReal();
            out += pad + "<real>" + s.str() + "</real>\n";
            break;
        }
        case Value::Type::String:
            out += pad + "<string>";
            escapeInto(out, v.asString());
            out += "</string>\n";
            break;
        case Value::Type::Data:
            out += pad + "<data>" + base64Encode(v.asString()) + "</data>\n";
            break;
        case Value::Type::Date:
            out += pad + "<date>";
            escapeInto(out, v.asString());
            out += "</date>\n";
            break;
        case Value::Type::Array:
            out += pad + "<array>\n";
            for (const auto& item : v.items()) writeValue(out, item, depth + 1);
            out += pad + "</array>\n";
            break;
        case Value::Type::Dict:
            out += pad + "<dict>\n";
            for (size_t i = 0; i < v.keys().size(); ++i) {
                out += pad + "\t<key>";
                escapeInto(out, v.keys()[i]);
                out += "</key>\n";
                writeValue(out, v.items()[i], depth + 1);
            }
            out += pad + "</dict>\n";
            break;
    }
}

}  // namespace

std::string toXml(const Value& root) {
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n";
    writeValue(out, root, 0);
    out += "</plist>\n";
    return out;
}

// ---------------------------------------------------------------- XML in

namespace {

std::optional<Value> parseNode(const pugi::xml_node& node, std::string* error) {
    const std::string_view name = node.name();
    const char* text = node.text().get();

    if (name == "dict") {
        Value dict = Value::dict();
        pugi::xml_node child = node.first_child();
        while (child) {
            if (child.type() != pugi::node_element) {
                child = child.next_sibling();
                continue;
            }
            if (std::string_view(child.name()) != "key") {
                if (error) *error = "expected <key> in <dict>, got <" + std::string(child.name()) + ">";
                return std::nullopt;
            }
            std::string key = child.text().get();
            pugi::xml_node valueNode = child.next_sibling();
            while (valueNode && valueNode.type() != pugi::node_element) valueNode = valueNode.next_sibling();
            if (!valueNode) {
                if (error) *error = "missing value for key " + key;
                return std::nullopt;
            }
            auto value = parseNode(valueNode, error);
            if (!value) return std::nullopt;
            dict.set(std::move(key), std::move(*value));
            child = valueNode.next_sibling();
        }
        return dict;
    }
    if (name == "array") {
        Value array = Value::array();
        for (pugi::xml_node child : node.children()) {
            if (child.type() != pugi::node_element) continue;
            auto value = parseNode(child, error);
            if (!value) return std::nullopt;
            array.push(std::move(*value));
        }
        return array;
    }
    if (name == "string") return Value::string(text);
    if (name == "date") return Value::date(text);
    if (name == "true") return Value::boolean(true);
    if (name == "false") return Value::boolean(false);
    if (name == "integer") {
        const std::string_view s = text;
        int64_t value = 0;
        auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
        if (ec != std::errc()) {
            uint64_t unsignedValue = 0;  // values above INT64_MAX (rare) wrap, like CFNumber does
            auto [ptr2, ec2] = std::from_chars(s.data(), s.data() + s.size(), unsignedValue);
            if (ec2 != std::errc()) {
                if (error) *error = "bad integer: " + std::string(s);
                return std::nullopt;
            }
            value = static_cast<int64_t>(unsignedValue);
        }
        return Value::integer(value);
    }
    if (name == "real") return Value::real(std::strtod(text, nullptr));
    if (name == "data") {
        auto bytes = base64Decode(text);
        if (!bytes) {
            if (error) *error = "bad base64 in <data>";
            return std::nullopt;
        }
        return Value::data(std::move(*bytes));
    }
    if (error) *error = "unsupported plist element <" + std::string(name) + ">";
    return std::nullopt;
}

}  // namespace

std::optional<Value> fromXml(std::string_view xml, std::string* error) {
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    if (!result) {
        if (error) *error = std::string("XML parse error: ") + result.description();
        return std::nullopt;
    }
    pugi::xml_node plistNode = doc.child("plist");
    if (!plistNode) {
        if (error) *error = "no <plist> root";
        return std::nullopt;
    }
    for (pugi::xml_node child : plistNode.children()) {
        if (child.type() == pugi::node_element) return parseNode(child, error);
    }
    if (error) *error = "empty <plist>";
    return std::nullopt;
}

// ---------------------------------------------------------------- describe

std::string describe(const Value& v, int indent) {
    const std::string pad(static_cast<size_t>(indent) * 2, ' ');
    switch (v.type()) {
        case Value::Type::None: return "null";
        case Value::Type::Bool: return v.asBool() ? "true" : "false";
        case Value::Type::Integer: return std::to_string(v.asInt());
        case Value::Type::Real: return std::to_string(v.asReal());
        case Value::Type::String: return "\"" + v.asString() + "\"";
        case Value::Type::Date: return v.asString();
        case Value::Type::Data: return "<" + std::to_string(v.asString().size()) + " bytes>";
        case Value::Type::Array: {
            if (v.items().empty()) return "[]";
            std::string out = "[\n";
            for (const auto& item : v.items()) out += pad + "  " + describe(item, indent + 1) + "\n";
            return out + pad + "]";
        }
        case Value::Type::Dict: {
            if (v.keys().empty()) return "{}";
            std::string out = "{\n";
            for (size_t i = 0; i < v.keys().size(); ++i) {
                out += pad + "  " + v.keys()[i] + ": " + describe(v.items()[i], indent + 1) + "\n";
            }
            return out + pad + "}";
        }
    }
    return "?";
}

}  // namespace cabled::plist
