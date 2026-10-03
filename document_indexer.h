#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace document_index {
struct Document { std::string source, relativePath, title, type, content; };
struct Chunk { std::string source, file, title, section, chunkId, strategy, content, hash; std::vector<float> embedding; };
struct RunStats { int fileCount=0, chunkCount=0, totalChars=0, avgChunkChars=0, embeddingDim=0; long long elapsedMs=0; std::string indexedAt, status="not indexed", error; };
struct CorpusStats { int fileCount=0, totalLines=0; long long totalCharacters=0; };
struct SearchHit { double score=0; Chunk chunk; };

class DocumentIndexer {
public:
    DocumentIndexer();
    ~DocumentIndexer();
    std::string statusJson();
    bool start(const std::string& strategy, std::string& error);
    std::string searchJson(const std::string& query, const std::string& strategy, int topK);
    std::string compareJson(const std::string& query, int topK);
    static std::vector<Chunk> fixedChunks(const Document& document, size_t chunkSize=1400, size_t overlap=200);
    static std::vector<Chunk> structuralChunks(const Document& document, size_t maxChunk=2200);
    static double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static bool validIndexStrategy(const std::string& strategy);
    static bool validSearchRequest(const std::string& query, const std::string& strategy, int topK);
    static std::vector<size_t> rankVectors(const std::vector<float>& query, const std::vector<std::vector<float>>& vectors, int topK);
    static std::string databasePath();
    static CorpusStats inspectCorpus(const std::string& root);
    static bool embedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& vectors, std::string& error);
private:
    void runJob(std::string strategy);
    std::string dbPath_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool running_=false;
    std::string phase_, jobError_;
    int progress_=0, progressTotal_=0;
    CorpusStats corpus_;
};
}
