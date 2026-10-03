#include "document_indexer.h"
#include <sqlite3.h>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
using namespace document_index;
static void envSet(const char* k,const std::string& v) {
#ifdef _WIN32
    _putenv_s(k,v.c_str());
#else
    setenv(k,v.c_str(),1);
#endif
}
int main() {
    const auto corpus=DocumentIndexer::inspectCorpus(PROJECT_SOURCE_ROOT);
    assert(corpus.fileCount > 10 && corpus.fileCount < 100);
    Document d{"project_repo", "sample.cpp", "sample.cpp", "cpp", std::string(3000, 'x')};
    const auto fixed = DocumentIndexer::fixedChunks(d, 1000, 100);
    assert(fixed.size() == 4);
    assert(fixed.front().content.substr(900) == fixed[1].content.substr(0, 100));
    assert(fixed.back().content.size() == 300);
    assert(fixed[0].chunkId == "sample.cpp:fixed:0001" && fixed[0].strategy == "fixed");
    Document md{"project_repo", "guide.md", "guide.md", "markdown", "# Intro\nintro text\n## Setup\nsetup text\n"};
    auto structural = DocumentIndexer::structuralChunks(md);
    assert(structural.size() == 2 && structural[0].section == "Intro" && structural[1].section == "Setup");
    Document py{"project_repo", "sample.py", "sample.py", "python", "class A:\n    pass\ndef foo():\n    return 1\nasync def bar():\n    pass\n"};
    structural = DocumentIndexer::structuralChunks(py);
    assert(structural.size() == 3 && structural[0].section == "class A" && structural[1].section == "def foo" && structural[2].section == "async def bar");
    Document cpp{"project_repo", "sample.cpp", "sample.cpp", "cpp", "class A {\n};\nvoid foo() {\n}\n"};
    structural = DocumentIndexer::structuralChunks(cpp);
    assert(structural.size() >= 2 && structural[0].section.find("class") == 0);
    assert(std::abs(DocumentIndexer::cosineSimilarity({1,0},{0.8f,0.6f}) - 0.8) < 1e-5);
    assert(DocumentIndexer::cosineSimilarity({1,0},{0,1}) < 1e-5);
    assert(DocumentIndexer::validIndexStrategy("both") && !DocumentIndexer::validIndexStrategy("unknown"));
    assert(DocumentIndexer::validSearchRequest("q", "fixed", 5));
    assert(!DocumentIndexer::validSearchRequest("", "fixed", 5));
    assert(!DocumentIndexer::validSearchRequest("q", "bad", 5));
    assert(!DocumentIndexer::validSearchRequest("q", "fixed", 21));
    const auto ranked=DocumentIndexer::rankVectors({1,0},{{0.6f,0.8f},{1,0},{0,1}},2);
    assert(ranked.size()==2 && ranked[0]==1 && ranked[1]==0);

    const auto dbPath=(std::filesystem::temp_directory_path()/"document_index_unit.db").string();
    std::filesystem::remove(dbPath);envSet("DOCUMENT_INDEX_DB",dbPath);
    { DocumentIndexer index; }
    sqlite3* db=nullptr;assert(sqlite3_open(dbPath.c_str(),&db)==SQLITE_OK);
    assert(sqlite3_exec(db,"INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at) VALUES('fixed','project_repo','a.cpp','a.cpp','','a.cpp:fixed:0001','first','h',X'0000803F',1,'now')",nullptr,nullptr,nullptr)==SQLITE_OK);
    assert(sqlite3_exec(db,"DELETE FROM document_chunks WHERE strategy='fixed'; INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at) VALUES('fixed','project_repo','a.cpp','a.cpp','','a.cpp:fixed:0001','replacement','h2',X'00000040',1,'later')",nullptr,nullptr,nullptr)==SQLITE_OK);
    sqlite3_stmt* q=nullptr;assert(sqlite3_prepare_v2(db,"SELECT content,embedding,embedding_dim FROM document_chunks WHERE strategy='fixed'",-1,&q,nullptr)==SQLITE_OK);assert(sqlite3_step(q)==SQLITE_ROW);assert(std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,0)))=="replacement");assert(sqlite3_column_bytes(q,1)==4&&sqlite3_column_int(q,2)==1);sqlite3_finalize(q);sqlite3_close(db);std::filesystem::remove(dbPath);

    envSet("OLLAMA_URL","http://127.0.0.1:1");std::vector<std::vector<float>> embeddings;std::string error;
    assert(!DocumentIndexer::embedTexts({"mock query"},embeddings,error));assert(error=="Ollama unavailable");
    std::cout << "corpus files: " << corpus.fileCount << std::endl;
    std::cout << "document index unit tests passed" << std::endl;
}
