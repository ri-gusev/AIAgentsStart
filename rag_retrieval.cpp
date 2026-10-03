#include "rag_retrieval.h"
#include "json_value.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>

namespace rag {
bool Config::load(const std::string& path, Config& config, std::string& error) {
    error.clear(); config = Config{};
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "Could not read RAG configuration: " + path; return false; }
    const std::string text((std::istreambuf_iterator<char>(file)), {});
    app_json::JsonValue root;
    if (!app_json::JsonParser(text).parse(root, error)) { error = "Invalid RAG configuration JSON"; return false; }
    try {
        const auto readNumber = [&](const char* key) {
            const auto* value = root.member(key);
            if (!value || value->type != app_json::JsonValue::Type::Number) throw std::runtime_error(key);
            const double number = std::stod(value->text);
            if (!std::isfinite(number)) throw std::runtime_error(key);
            return number;
        };
        const double pre = readNumber("pre_filter_top_k"), post = readNumber("post_filter_top_k");
        const double threshold = readNumber("similarity_threshold");
        if (pre < 1 || pre > 20 || std::floor(pre) != pre || post < 1 || post > pre || std::floor(post) != post || threshold < 0 || threshold > 1)
            throw std::runtime_error("out of range");
        config.preFilterTopK = static_cast<int>(pre); config.postFilterTopK = static_cast<int>(post);
        config.similarityThreshold = threshold;
        return true;
    } catch (...) { error = "Invalid RAG parameters: top_k must be integers 1..20, post <= pre, threshold 0..1"; return false; }
}
namespace {
std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
bool wordMatch(const std::string& text, const std::string& term) {
    for (size_t at = text.find(term); at != std::string::npos; at = text.find(term, at + 1)) {
        const auto identifier = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
        if ((at == 0 || !identifier(text[at-1])) &&
            (at + term.size() == text.size() || !identifier(text[at+term.size()]))) return true;
    }
    return false;
}
}
std::vector<document_index::SearchHit> selectCandidates(const std::string& rewrittenQuery,
        std::vector<document_index::SearchHit> candidates, const Config& config) {
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [](const auto& hit) { return !std::isfinite(hit.score); }), candidates.end());
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (candidates.size() > static_cast<size_t>(config.preFilterTopK)) candidates.resize(config.preFilterTopK);
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const auto& hit) {
        return hit.score < config.similarityThreshold;
    }), candidates.end());
    const std::set<std::string> stop = {"the","and","with","what","where","which","does","how","from","for","that","this","code","document","documents"};
    const std::regex identifiers(R"([A-Za-z_][A-Za-z0-9_:.\-]*)");
    std::set<std::string> terms, entities;
    for (std::sregex_iterator it(rewrittenQuery.begin(), rewrittenQuery.end(), identifiers), end; it != end; ++it) {
        const std::string token = it->str(), normalized = lower(token);
        if (normalized.size() < 3 || stop.count(normalized)) continue;
        terms.insert(normalized);
        if (token.find('_') != token.npos || token.find("::") != token.npos ||
            std::any_of(token.begin()+1, token.end(), [](unsigned char c) { return std::isupper(c); })) entities.insert(token);
    }
    for (auto& hit : candidates) {
        const auto& chunk = hit.chunk;
        const std::string file = lower(chunk.file), section = lower(chunk.section), content = lower(chunk.content);
        bool fileMatch = false, sectionMatch = false, entityMatch = false;
        int matchedTerms = 0;
        for (const auto& term : terms) {
            fileMatch = fileMatch || file.find(term) != file.npos;
            sectionMatch = sectionMatch || wordMatch(section, term);
            if (wordMatch(content, term)) ++matchedTerms;
        }
        for (const auto& entity : entities) entityMatch = entityMatch || wordMatch(chunk.section, entity) || wordMatch(chunk.content, entity);
        const double bonus = std::min(0.12, (fileMatch ? 0.04 : 0) + (sectionMatch ? 0.05 : 0) +
            std::min(0.03, matchedTerms * 0.01) + (entityMatch ? 0.04 : 0));
        hit.relevanceScore = std::min(1.0, hit.score + bonus);
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.relevanceScore > b.relevanceScore;
    });
    if (candidates.size() > static_cast<size_t>(config.postFilterTopK)) candidates.resize(config.postFilterTopK);
    return candidates;
}
}
