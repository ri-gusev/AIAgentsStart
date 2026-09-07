#include "agent.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {
std::string jsonEscape(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += c < 0x20 ? "?" : std::string(1, static_cast<char>(c));
        }
    }
    return result;
}

std::string extractJsonStringField(const std::string& json, const std::string& field) {
    const auto key = json.find("\"" + field + "\"");
    if (key == std::string::npos) return {};
    auto start = json.find(':', key + field.size() + 2);
    if (start == std::string::npos) return {};
    do { ++start; } while (start < json.size() && std::isspace(static_cast<unsigned char>(json[start])));
    if (start >= json.size() || json[start++] != '"') return {};

    std::string result;
    bool escaped = false;
    for (; start < json.size(); ++start) {
        const char c = json[start];
        if (escaped) {
            switch (c) {
            case 'n': result += '\n'; break;
            case 'r': result += '\r'; break;
            case 't': result += '\t'; break;
            case 'b': result += '\b'; break;
            case 'f': result += '\f'; break;
            default: result += c; break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            return result;
        } else {
            result += c;
        }
    }
    return {};
}

bool extractJsonBoolField(const std::string& json, const std::string& field, bool& value) {
    const auto key = json.find("\"" + field + "\"");
    if (key == std::string::npos) return false;
    const auto colon = json.find(':', key + field.size() + 2);
    if (colon == std::string::npos) return false;
    const auto start = json.find_first_not_of(" \t\r\n", colon + 1);
    if (start == std::string::npos) return false;
    if (json.compare(start, 4, "true") == 0) { value = true; return true; }
    if (json.compare(start, 5, "false") == 0) { value = false; return true; }
    return false;
}

}

Agent::Agent(const std::string& configPath) {
    if (!apiClient_.isReady()) {
        initializationError_ = "Could not initialize libcurl";
        return;
    }
    if (!loadConfig(configPath, initializationError_)) return;
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        initializationError_ = "OPENAI_API_KEY is not set";
        return;
    }
    apiKey_ = key;
}

bool Agent::isReady() const { return initializationError_.empty(); }
const std::string& Agent::initializationError() const { return initializationError_; }
const std::string& Agent::modelName() const { return config_.model; }

bool Agent::loadConfig(const std::string& configPath, std::string& error) {
    std::ifstream file(configPath, std::ios::binary);
    if (!file) { error = "Could not open agent configuration: " + configPath; return false; }
    const std::string json((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    config_.model = extractJsonStringField(json, "model");
    config_.inputPolicy = extractJsonStringField(json, "input_policy");
    config_.outputPolicy = extractJsonStringField(json, "output_policy");
    const auto memoryKey = json.find("\"short_term_memory_turns\"");
    if (memoryKey != std::string::npos) {
        const auto colon = json.find(':', memoryKey);
        if (colon != std::string::npos) {
            try { config_.shortTermMemoryTurns = std::stoul(json.substr(colon + 1)); }
            catch (...) { error = "short_term_memory_turns must be a positive integer"; return false; }
        }
    }
    if (config_.model.empty() || config_.outputPolicy.empty() || config_.shortTermMemoryTurns == 0) {
        error = "Agent configuration requires model, output_policy, and a positive short_term_memory_turns";
        return false;
    }
    return true;
}

std::string Agent::buildConversation(const std::string& userMessage) const {
    std::string messages = "[";
    bool first = true;
    const auto append = [&messages, &first](const char* role, const std::string& content) {
        if (!first) messages += ',';
        first = false;
        messages += "{\"role\":\"" + std::string(role) + "\",\"content\":\"" + jsonEscape(content) + "\"}";
    };
    if (!config_.inputPolicy.empty()) append("system", config_.inputPolicy);
    for (const auto& turn : shortTermMemory_) { append("user", turn.user); append("assistant", turn.assistant); }
    append("user", userMessage);
    return messages + "]";
}

std::string Agent::buildReviewConversation(const std::string& userMessage,
                                           const std::string& draft) const {
    std::string messages = "[";
    bool first = true;
    const auto append = [&messages, &first](const char* role, const std::string& content) {
        if (!first) messages += ',';
        first = false;
        messages += "{\"role\":\"" + std::string(role) + "\",\"content\":\"" + jsonEscape(content) + "\"}";
    };

    if (!config_.inputPolicy.empty()) append("system", config_.inputPolicy);
    for (const auto& turn : shortTermMemory_) {
        append("user", turn.user);
        append("assistant", turn.assistant);
    }
    append("user", userMessage);
    append("assistant", draft);
    append("system", config_.outputPolicy +
        " Return ONLY valid JSON: {\"accepted\":true} if the draft complies, "
        "or {\"accepted\":false,\"revised_answer\":\"...\"} if it must be corrected.");
    return messages + "]";
}

bool Agent::reviewAnswer(const std::string& userMessage, const std::string& draft,
                         std::string& finalAnswer, std::string& error) const {
    std::string review;
    if (!apiClient_.sendChatCompletion(apiKey_, config_.model,
                                       buildReviewConversation(userMessage, draft),
                                       review, error)) return false;
    bool accepted = false;
    if (!extractJsonBoolField(review, "accepted", accepted)) {
        error = "Output-policy review did not return valid JSON";
        return false;
    }
    if (accepted) { finalAnswer = draft; return true; }
    finalAnswer = extractJsonStringField(review, "revised_answer");
    if (finalAnswer.empty()) { error = "Output-policy review did not provide revised_answer"; return false; }
    return true;
}

void Agent::remember(const std::string& userMessage, const std::string& answer) {
    shortTermMemory_.push_back({userMessage, answer});
    if (shortTermMemory_.size() > config_.shortTermMemoryTurns)
        shortTermMemory_.erase(shortTermMemory_.begin());
}

bool Agent::respond(const std::string& userMessage, std::string& answer, std::string& error) {
    if (!isReady()) { error = initializationError_; return false; }
    std::string draft;
    if (!apiClient_.sendChatCompletion(apiKey_, config_.model, buildConversation(userMessage), draft, error)) return false;
    if (!reviewAnswer(userMessage, draft, answer, error)) return false;
    remember(userMessage, answer);
    return true;
}
