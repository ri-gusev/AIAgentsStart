#pragma once

#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace document_index {
struct Document { std::string source, relativePath, title, type, content; int page=0; };
struct Chunk { std::string source, file, title, section, chunkId, strategy, content, hash; std::vector<float> embedding; std::string sourceType="project", uploadId; int page=0, lineStart=0, lineEnd=0; };
struct CorpusStats { int fileCount=0, totalLines=0; long long totalCharacters=0; };
struct SearchHit { double score=0; Chunk chunk; double relevanceScore=0; };

class DocumentIndexer {
public:
    DocumentIndexer();
    ~DocumentIndexer();
    std::string statusJson();
    bool start(std::string& error);
    bool ensureReady(bool& ready, std::string& error);
    bool retrieve(const std::string& query, int topK, std::vector<SearchHit>& hits, std::string& error);
    bool upload(const std::string& name, const std::string& bytes, std::string& id, std::string& error);
    std::string uploadsJson();
    static std::vector<Chunk> structuralChunks(const Document& document, size_t maxChunk=2200);
    static double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);
    static bool validSearchRequest(const std::string& query, int topK);
    static std::vector<size_t> rankVectors(const std::vector<float>& query, const std::vector<std::vector<float>>& vectors, int topK);
    static std::string databasePath();
    static CorpusStats inspectCorpus(const std::string& root);
    static bool embedTexts(const std::vector<std::string>& texts, std::vector<std::vector<float>>& vectors, std::string& error);
private:
    void runJob();
    void runUploads();
    void indexUpload(const std::string& id);
    void resumeUploads();
    bool hasIndex(std::string& error) const;
    std::string dbPath_;
    mutable std::mutex mutex_;
    std::thread worker_;
    std::thread uploadWorker_;
    std::condition_variable uploadsWake_;
    std::deque<std::string> pendingUploads_;
    bool uploadsStopped_=false;
    bool uploadsResumed_=false;
    bool running_=false;
    std::string phase_, jobError_;
    int progress_=0, progressTotal_=0;
    CorpusStats corpus_;
};
}
