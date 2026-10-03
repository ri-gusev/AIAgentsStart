#include "rag_grounding.h"
#include "rag_retrieval.h"
#include <iostream>
#include <stdexcept>

void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        document_index::SearchHit hit;
        hit.score = .8; hit.relevanceScore = .85;
        hit.chunk.file = "mcp_manager.cpp"; hit.chunk.section = "McpManager::callTool";
        hit.chunk.chunkId = "code:42"; hit.chunk.sourceType = "project";
        hit.chunk.lineStart = 10; hit.chunk.lineEnd = 14;
        hit.chunk.content = "void McpManager::callTool() {\n    dispatch();\n}\n";
        hit.chunk.embedding = {45678.f};
        auto uploaded = hit; uploaded.chunk.file = "manual.pdf"; uploaded.chunk.chunkId = "upload:7";
        uploaded.chunk.sourceType = "upload"; uploaded.chunk.page = 14; uploaded.chunk.lineStart = uploaded.chunk.lineEnd = 0;
        uploaded.chunk.content = "The scheduler runs a worker thread.";
        rag::GroundingContext context; rag::GroundedAnswer answer; std::string error;
        require(rag::readGroundingContext(rag::buildContext({hit,uploaded}), context, error), "Read actual retrieved text and metadata");
        require(rag::validateGroundedAnswer(R"({"answer":"Dispatch calls dispatch().","sources":[{"chunk_id":"code:42"}],"quotes":[{"chunk_id":"code:42","text":"dispatch();"}]})", context, answer, error), "Valid grounded result");
        require(answer.sources.size() == 1 && answer.quotes.size() == 1 && answer.quotes[0].language == "cpp" &&
                answer.sources[0].lineStart == 10 && answer.sources[0].relevanceScore == .85, "Canonical code quote and metadata");
        require(answer.quotes[0].text == hit.chunk.content && answer.quotes[0].text.find("45678") == std::string::npos, "Backend quotes exact text, no embeddings");
        require(rag::validateGroundedAnswer(R"({"answer":"Worker thread.","sources":[{"chunk_id":"upload:7"}]})", context, answer, error) && answer.sources[0].page == 14 && answer.quotes[0].language.empty(), "Uploaded PDF evidence");
        for (const auto* invalid : {
            R"({"answer":"Fake","sources":[{"chunk_id":"invented"}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42","file":"invented.cpp"}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42","section":"wrong"}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"upload:7","page":15}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42","score":0.99}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42"}],"quotes":[{"chunk_id":"code:42","text":"dispatchEverything();"}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42"}],"quotes":[{"chunk_id":"upload:7","text":"worker thread"}]})",
            R"({"answer":"Fake","sources":[{"chunk_id":"code:42"},{"chunk_id":"code:42"}]})",
            R"({"answer":"Fake","sources":[]})",
            "Unstructured answer with invented quotations"}) {
            require(!rag::validateGroundedAnswer(invalid, context, answer, error) && answer.sources.empty() && answer.quotes.empty(), "Reject unsupported source/quote claims");
        }
        require(rag::validateGroundedAnswer(R"({"insufficient_context":true})", context, answer, error) && answer.answer == rag::dontKnowAnswer() && answer.sources.empty(), "Insufficient evidence even with semantic candidates");
        require(rag::validateGroundedAnswer(R"({"answer":"Invented","sources":[]})", {}, answer, error) && answer.answer == rag::dontKnowAnswer(), "No chunks cannot yield a general-knowledge answer");
        require(rag::validateGroundedAnswer(R"({"answer":"Valid","sources":["code:42"]})", context, answer, error), "Canonical source selection");
        answer.quotes[0].text = "modified quotation";
        require(!rag::validateEvidence(answer,context), "Post-answer evidence verification");
        std::cout << "PASS: grounded answer, code/PDF quotes, metadata, invalid citations, quote tampering, insufficient/no evidence\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
