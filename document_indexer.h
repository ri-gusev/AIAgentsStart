#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace document_index {
struct Document { std::string source, relativePath, title, type, content; };
struct Chunk { std::string source, file, title, section, chunkId, strategy, content, hash; std::vector<float> embedding; };
struct CorpusStats { int fileCount=0, totalLines=0; long long totalCharacters=0; };
struct SearchHit { double score=0; Chunk chunk; };

class DocumentIndexer {
public:
    DocumentIndexer();
    ~DocumentIndexer();
    std::string statusJson();
    bool start(std::string& error);
    bool ensureReady(bool& ready, std::string& error);
    bool retrieve(const std::string& query, int topK, std::vector<SearchHit>& hits, std::string& error);
    static std::vector<Chunk> structuralChunks(const Document& document, size_t maxChunk=2200);
    static double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static bool validSearchRequest(const std::string& query, int topK);
    static std::vector<size_t> rankVectors(const std::vector<float>& query, const std::vector<std::vector<float>>& vectors, int topK);
    static std::string databasePath();
    static CorpusStats inspectCorpus(const std::string& root);
    static bool embedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& vectors, std::string& error);
private:
    void runJob();
    bool hasIndex(std::string& error) const;
    std::string dbPath_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool running_=false;
    std::string phase_, jobError_;
    int progress_=0, progressTotal_=0;
    CorpusStats corpus_;
};
}
