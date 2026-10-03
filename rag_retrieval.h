#pragma once
#include "document_indexer.h"

namespace rag {
struct Config {
    int preFilterTopK = 10;
    int postFilterTopK = 4;
    double similarityThreshold = 0.45;
    static bool load(const std::string& path, Config& config, std::string& error);
};
// A bounded lexical bonus refines semantic ranking; filtering uses cosine only.
std::vector<document_index::SearchHit> selectCandidates(const std::string& rewrittenQuery,
    std::vector<document_index::SearchHit> candidates, const Config& config);
// Text + source metadata only. Embeddings never enter the model context.
std::string buildContext(const std::vector<document_index::SearchHit>& hits);
}
