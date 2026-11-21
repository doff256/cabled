#pragma once
// Tiny XML property-list model: just enough for the usbmux protocol (D-02).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabled::plist {

class Value {
public:
    enum class Type { None, Bool, Integer, Real, String, Data, Date, Array, Dict };

    Value() = default;
    static Value boolean(bool b);
    static Value integer(int64_t i);
    static Value real(double d);
    static Value string(std::string s);
    static Value data(std::string bytes);  // raw bytes, base64-encoded on output
    static Value date(std::string iso8601);
    static Value array();
    static Value dict();

    Type type() const { return type_; }
    bool is(Type t) const { return type_ == t; }

    bool asBool(bool fallback = false) const;
    int64_t asInt(int64_t fallback = 0) const;
    double asReal(double fallback = 0.0) const;
    // String, Data and Date payloads; empty for other types.
    const std::string& asString() const { return s_; }

    // Array
    Value& push(Value v);
    const std::vector<Value>& items() const { return items_; }

    // Dict (keeps insertion order). For dicts, items() holds the values parallel to keys().
    Value& set(std::string key, Value v);
    const Value* find(std::string_view key) const;
    const std::vector<std::string>& keys() const { return keys_; }

    size_t size() const { return items_.size(); }

private:
    Type type_ = Type::None;
    bool b_ = false;
    int64_t i_ = 0;
    double d_ = 0.0;
    std::string s_;
    std::vector<std::string> keys_;
    std::vector<Value> items_;
};

std::string toXml(const Value& root);
std::optional<Value> fromXml(std::string_view xml, std::string* error = nullptr);

// Human-readable dump, for logs and the probe tool.
std::string describe(const Value& v, int indent = 0);

std::string base64Encode(std::string_view bytes);
std::optional<std::string> base64Decode(std::string_view text);

}  // namespace cabled::plist
