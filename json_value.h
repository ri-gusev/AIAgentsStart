#pragma once
#include <cctype>
#include <map>
#include <string>
#include <vector>
#include <utility>

// Shared existing parser; MCP transport semantics remain unchanged.
namespace app_json {
struct JsonValue {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    std::string text;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    const JsonValue* member(const std::string& key) const {
        const auto found = object.find(key);
        return found == object.end() ? nullptr : &found->second;
    }
};

inline void appendUtf8(std::string& output, unsigned codePoint) {
    if (codePoint <= 0x7F) output += static_cast<char>(codePoint);
    else if (codePoint <= 0x7FF) {
        output += static_cast<char>(0xC0 | (codePoint >> 6));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint <= 0xFFFF) {
        output += static_cast<char>(0xE0 | (codePoint >> 12));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        output += static_cast<char>(0xF0 | (codePoint >> 18));
        output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        output += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    bool parse(JsonValue& value, std::string& error) {
        skipWhitespace();
        if (!parseValue(value)) {
            error = "Invalid JSON in MCP response at byte " + std::to_string(position_);
            return false;
        }
        skipWhitespace();
        if (position_ != input_.size()) {
            error = "Unexpected data after MCP JSON response";
            return false;
        }
        return true;
    }

private:
    void skipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_;
    }

    bool parseValue(JsonValue& value) {
        skipWhitespace();
        if (position_ >= input_.size()) return false;
        const char current = input_[position_];
        if (current == '"') {
            value.type = JsonValue::Type::String;
            return parseString(value.text);
        }
        if (current == '{') return parseObject(value);
        if (current == '[') return parseArray(value);
        if (current == 't' && match("true")) {
            value.type = JsonValue::Type::Boolean; value.boolean = true; return true;
        }
        if (current == 'f' && match("false")) {
            value.type = JsonValue::Type::Boolean; value.boolean = false; return true;
        }
        if (current == 'n' && match("null")) {
            value.type = JsonValue::Type::Null; return true;
        }
        return parseNumber(value);
    }

    bool parseObject(JsonValue& value) {
        value = JsonValue{};
        value.type = JsonValue::Type::Object;
        ++position_;
        skipWhitespace();
        if (consume('}')) return true;
        while (position_ < input_.size()) {
            std::string key;
            if (!parseString(key)) return false;
            skipWhitespace();
            if (!consume(':')) return false;
            JsonValue child;
            if (!parseValue(child)) return false;
            value.object[std::move(key)] = std::move(child);
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(JsonValue& value) {
        value = JsonValue{};
        value.type = JsonValue::Type::Array;
        ++position_;
        skipWhitespace();
        if (consume(']')) return true;
        while (position_ < input_.size()) {
            JsonValue child;
            if (!parseValue(child)) return false;
            value.array.push_back(std::move(child));
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseString(std::string& output) {
        output.clear();
        if (!consume('"')) return false;
        while (position_ < input_.size()) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return true;
            if (current < 0x20) return false;
            if (current != '\\') {
                output += static_cast<char>(current);
                continue;
            }
            if (position_ >= input_.size()) return false;
            const char escaped = input_[position_++];
            switch (escaped) {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                unsigned codePoint = 0;
                if (!readHex4(codePoint)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                    if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1] != 'u') return false;
                    position_ += 2;
                    unsigned low = 0;
                    if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + low - 0xDC00;
                } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return false;
                appendUtf8(output, codePoint);
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool readHex4(unsigned& value) {
        if (input_.size() - position_ < 4) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const unsigned char current = static_cast<unsigned char>(input_[position_++]);
            unsigned digit = 0;
            if (current >= '0' && current <= '9') digit = current - '0';
            else if (current >= 'a' && current <= 'f') digit = current - 'a' + 10;
            else if (current >= 'A' && current <= 'F') digit = current - 'A' + 10;
            else return false;
            value = value * 16 + digit;
        }
        return true;
    }

    bool parseNumber(JsonValue& value) {
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        if (position_ >= input_.size()) return false;
        if (input_[position_] == '0') ++position_;
        else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size() && input_[position_] == '.') {
            ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            if (position_ >= input_.size() ||
                !std::isdigit(static_cast<unsigned char>(input_[position_]))) return false;
            while (position_ < input_.size() &&
                   std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        value.type = JsonValue::Type::Number;
        value.text = input_.substr(start, position_ - start);
        return position_ > start;
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool match(const char* text) {
        const std::string value(text);
        if (input_.compare(position_, value.size(), value) != 0) return false;
        position_ += value.size();
        return true;
    }

    const std::string& input_;
    size_t position_ = 0;
};

inline std::string jsonEscape(const std::string& value) {
    std::string result;
    for (unsigned char current : value) {
        switch (current) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += current < 0x20 ? "?" : std::string(1, static_cast<char>(current));
        }
    }
    return result;
}

inline std::string serializeJson(const JsonValue& value) {
    switch (value.type) {
    case JsonValue::Type::Null: return "null";
    case JsonValue::Type::Boolean: return value.boolean ? "true" : "false";
    case JsonValue::Type::Number: return value.text;
    case JsonValue::Type::String: return "\"" + jsonEscape(value.text) + "\"";
    case JsonValue::Type::Array: {
        std::string result = "[";
        bool first = true;
        for (const auto& child : value.array) {
            if (!first) result += ',';
            first = false;
            result += serializeJson(child);
        }
        return result + ']';
    }
    case JsonValue::Type::Object: {
        std::string result = "{";
        bool first = true;
        for (const auto& entry : value.object) {
            if (!first) result += ',';
            first = false;
            result += "\"" + jsonEscape(entry.first) + "\":" + serializeJson(entry.second);
        }
        return result + '}';
    }
    }
    return "null";
}

}
