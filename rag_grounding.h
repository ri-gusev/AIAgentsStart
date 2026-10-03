#pragma once
#include <string>
#include <vector>

namespace rag {
struct Source {
    std::string file, section, chunkId, sourceType;
    int page=0, lineStart=0, lineEnd=0;
    double relevanceScore=0;
};
struct Quote { std::string chunkId, text, language; };
struct Evidence { Source source; std::string text; };
struct GroundingContext { std::vector<Evidence> chunks; };
struct GroundedAnswer { std::string answer; std::vector<Source> sources; std::vector<Quote> quotes; };
const char* dontKnowAnswer();
const char* groundedInstructions();
bool readGroundingContext(const std::string& json, GroundingContext& context, std::string& error);
// Only IDs and optional quotes from the model are checked. Metadata and displayed
// quotes are reconstructed from the backend's retrieved text, never trusted from LLM.
bool validateGroundedAnswer(const std::string& json, const GroundingContext& context,
                           GroundedAnswer& result, std::string& error);
bool validateEvidence(const GroundedAnswer& answer, const GroundingContext& context);
}
