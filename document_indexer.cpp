#include "document_indexer.h"
#include "json_value.h"

#include <curl/curl.h>
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#endif

namespace document_index {
namespace fs=std::filesystem;
namespace {
constexpr size_t kBatchSize=32, kMaxFileBytes=1024*1024;
std::string env(const char* key,const char* fallback) { const char* v=std::getenv(key); return v&&*v?v:fallback; }
std::string trim(std::string s) { const auto a=s.find_first_not_of(" \t\r\n"); if(a==s.npos)return {}; return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1); }
std::string escape(const std::string& s) { return app_json::jsonEscape(s); }
std::string nowUtc() { const auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); std::tm tm{};
#ifdef _WIN32
 gmtime_s(&tm,&t);
#else
 gmtime_r(&t,&tm);
#endif
 std::ostringstream o;o<<std::put_time(&tm,"%Y-%m-%dT%H:%M:%SZ");return o.str(); }
std::string hashText(const std::string& s) { uint64_t h=1469598103934665603ULL; for(unsigned char c:s){h^=c;h*=1099511628211ULL;} std::ostringstream o;o<<std::hex<<h;return o.str(); }
std::string lang(const fs::path& p) { auto e=p.extension().string(); if(e==".cpp"||e==".h"||e==".hpp"||e==".c")return "cpp"; if(e==".py")return "python"; if(e==".md")return "markdown";if(e==".js")return "javascript";return "text"; }
std::string basename(const std::string& p) { return fs::path(p).filename().string(); }
size_t writeBody(char* ptr,size_t size,size_t nmemb,void* ctx){auto* s=static_cast<std::string*>(ctx);s->append(ptr,size*nmemb);return size*nmemb;}
bool curlRequest(const std::string& url,const std::string& body,std::string& response,std::string& error) {
 CURL* c=curl_easy_init(); if(!c){error="Ollama unavailable";return false;} struct curl_slist* headers=nullptr;headers=curl_slist_append(headers,"Content-Type: application/json");
 curl_easy_setopt(c,CURLOPT_URL,url.c_str());curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(c,CURLOPT_POST,1L);curl_easy_setopt(c,CURLOPT_POSTFIELDS,body.data());curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,writeBody);curl_easy_setopt(c,CURLOPT_WRITEDATA,&response);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,2L);curl_easy_setopt(c,CURLOPT_TIMEOUT,90L);
 const CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_slist_free_all(headers);curl_easy_cleanup(c);
 if(rc!=CURLE_OK||status<200||status>=300){error= (rc==CURLE_COULDNT_CONNECT||rc==CURLE_OPERATION_TIMEDOUT)?"Ollama unavailable":"Ollama embedding request failed";return false;} return true;
}
std::vector<Document> loadDocuments(const fs::path& root, CorpusStats& stats) {
    static const std::vector<std::string> allowed={".md",".txt",".cpp",".h",".hpp",".c",".py",".js",".html",".css"};
    static const std::vector<std::string> excluded={".git","build",".venv",".venv-mcp",".venv-mcp-run","node_modules","__pycache__","generated","dist","tests",".vscode","vendor","third_party"};
    std::vector<Document> out;
    for(fs::recursive_directory_iterator it(root,fs::directory_options::skip_permission_denied),end;it!=end;) {
        const auto entry=*it;
        const std::string relative=fs::relative(entry.path(),root).generic_string();
        if(entry.is_directory()) {
            bool skip=false;
            for(const auto& part:fs::path(relative)) {
                if(std::find(excluded.begin(),excluded.end(),part.string())!=excluded.end()) { skip=true; break; }
            }
            if(skip) it.disable_recursion_pending();
            std::error_code iterError;
            it.increment(iterError);
            continue;
        }
        std::error_code iterError;
        it.increment(iterError);
        if(!entry.is_regular_file() || entry.is_symlink()) continue;
        std::error_code sizeError;
        const auto bytes=entry.file_size(sizeError);
        if(sizeError||bytes>kMaxFileBytes) continue;
        const auto ext=entry.path().extension().string();
        if(std::find(allowed.begin(),allowed.end(),ext)==allowed.end()) continue;
        const auto relativePath=fs::path(relative);
        const auto first=relativePath.begin()==relativePath.end()?std::string{}:relativePath.begin()->string();
        const bool atRoot=relativePath.parent_path().empty();
        const bool coreCpp=atRoot&&(ext==".cpp"||ext==".h"||ext==".hpp"||ext==".c");
        const bool coreUi=atRoot&&(ext==".js"||ext==".html"||ext==".css"||relative=="CMakeLists.txt"||relative=="rag_extract.py");
        const bool coreReadme=(relative=="README.md"||((first=="mcp_server"||first=="codeforces_mcp_server")&&entry.path().filename()=="README.md"));
        const bool mcpPython=(first=="mcp_server"||first=="codeforces_mcp_server")&&ext==".py";
        if(!coreCpp&&!coreUi&&!coreReadme&&!mcpPython) continue;
        const auto fileName=entry.path().filename().string();
        if(fileName.find(".generated.")!=std::string::npos||fileName.find("_generated.")!=std::string::npos||fileName.find(".min.")!=std::string::npos||fileName.find("autogen.")!=std::string::npos) continue;
        std::ifstream file(entry.path(),std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)),{});
        if(content.find('\0')!=std::string::npos) continue;
        Document document{"project_repo",relative,basename(relative),lang(entry.path()),std::move(content)};
        ++stats.fileCount;
        stats.totalCharacters+=document.content.size();
        stats.totalLines+=static_cast<int>(std::count(document.content.begin(),document.content.end(),'\n'))+
            (!document.content.empty()&&document.content.back()!='\n');
        out.push_back(std::move(document));
    }
    return out;
}

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

void exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string error = message ? message : sqlite3_errmsg(db);
        sqlite3_free(message);
        throw std::runtime_error(error);
    }
}
Database openDb(const std::string& path) {
    static std::mutex schemaMutex;
    std::lock_guard<std::mutex> schemaLock(schemaMutex);
    sqlite3* raw = nullptr;
    const int result = sqlite3_open(path.c_str(), &raw);
    Database db(raw, sqlite3_close);
    if (result != SQLITE_OK) throw std::runtime_error("Could not open document index database");
    sqlite3_busy_timeout(db.get(), 3000);
    exec(db.get(), "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS document_chunks(id INTEGER PRIMARY KEY,strategy TEXT NOT NULL,"
        "source TEXT NOT NULL,file TEXT NOT NULL,title TEXT NOT NULL,section TEXT NOT NULL,"
        "chunk_id TEXT NOT NULL,content TEXT NOT NULL,content_hash TEXT NOT NULL,embedding BLOB NOT NULL,"
        "embedding_dim INTEGER NOT NULL,indexed_at TEXT NOT NULL,UNIQUE(strategy,chunk_id));"
        "CREATE TABLE IF NOT EXISTS indexing_runs(strategy TEXT PRIMARY KEY,file_count INTEGER,"
        "chunk_count INTEGER,total_chars INTEGER,avg_chunk_chars INTEGER,embedding_dim INTEGER,"
        "elapsed_ms INTEGER,indexed_at TEXT,status TEXT,error TEXT);"
        "DELETE FROM document_chunks WHERE strategy<>'structural';"
        "DELETE FROM indexing_runs WHERE strategy<>'structural';");
    const std::vector<std::pair<std::string,std::string>> columns = {
        {"source_type", "TEXT NOT NULL DEFAULT 'project'"}, {"upload_id", "TEXT NOT NULL DEFAULT ''"},
        {"page", "INTEGER NOT NULL DEFAULT 0"}, {"line_start", "INTEGER NOT NULL DEFAULT 0"},
        {"line_end", "INTEGER NOT NULL DEFAULT 0"}};
    for (const auto& column : columns) {
        sqlite3_stmt* raw = nullptr;
        if (sqlite3_prepare_v2(db.get(), "PRAGMA table_info(document_chunks);", -1, &raw, nullptr) != SQLITE_OK)
            throw std::runtime_error("Could not read index schema");
        Statement statement(raw, sqlite3_finalize);
        bool found = false;
        while (sqlite3_step(statement.get()) == SQLITE_ROW)
            if (column.first == reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 1))) found = true;
        statement.reset();
        if (!found) exec(db.get(), ("ALTER TABLE document_chunks ADD COLUMN " + column.first + " " + column.second).c_str());
    }
    exec(db.get(), "CREATE TABLE IF NOT EXISTS rag_uploads(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "file_name TEXT NOT NULL,type TEXT NOT NULL,data BLOB NOT NULL,status TEXT NOT NULL DEFAULT 'uploaded',"
        "error TEXT NOT NULL DEFAULT '',created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);");
    return db;
}
Statement prepare(sqlite3* db, const char* sql) {
    sqlite3_stmt* raw = nullptr;
    const int result = sqlite3_prepare_v2(db, sql, -1, &raw, nullptr);
    Statement statement(raw, sqlite3_finalize);
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
    return statement;
}
void bindText(sqlite3_stmt* statement, int position, const std::string& value) {
    if (sqlite3_bind_text(statement, position, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("Could not bind index text");
}
void done(sqlite3* db, sqlite3_stmt* statement) {
    if (sqlite3_step(statement) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
}
std::string columnText(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}
bool indexExists(sqlite3* db) {
    auto statement = prepare(db, "SELECT COUNT(*) FROM document_chunks WHERE strategy='structural' AND source_type='project';");
    if (sqlite3_step(statement.get()) != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    return sqlite3_column_int64(statement.get(), 0) > 0;
}
}

DocumentIndexer::DocumentIndexer() : dbPath_(databasePath()) {}
DocumentIndexer::~DocumentIndexer() {
    { std::lock_guard<std::mutex> lock(mutex_); uploadsStopped_ = true; }
    uploadsWake_.notify_all();
    if (uploadWorker_.joinable()) uploadWorker_.join();
    if (worker_.joinable()) worker_.join();
}
std::string DocumentIndexer::databasePath() { return env("DOCUMENT_INDEX_DB", "document_index.db"); }
CorpusStats DocumentIndexer::inspectCorpus(const std::string& root) {
    CorpusStats stats; loadDocuments(root, stats); return stats;
}

std::vector<Chunk> DocumentIndexer::structuralChunks(const Document& document, size_t maxChunk) {
    std::vector<Chunk> chunks;
    std::string section = document.title, buffer, line;
    std::istringstream input(document.content);
    int lineNumber = 0, firstLine = 1;
    const std::regex markdown(R"(^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$)");
    const std::regex python(R"(^\s*(async\s+def|def|class)\s+([A-Za-z_][A-Za-z0-9_]*))");
    const std::regex declaration(R"(^\s*(namespace|class|struct)\s+([A-Za-z_][A-Za-z0-9_:]*))");
    const std::regex function(R"(^\s*(?:[\w:<>&*]+\s+)+([A-Za-z_~][\w:~]*)\s*\([^;]*\)\s*(?:const\s*)?(?:\{|$))");
    const std::regex javascript(R"(^\s*(?:(async\s+)?function|class)\s+([A-Za-z_$][\w$]*))");
    const auto flush = [&] {
        const std::string text = trim(buffer);
        buffer.clear();
        if (text.empty()) return;
        std::ostringstream id;
        id << document.relativePath << ":structural:" << std::setw(4) << std::setfill('0') << chunks.size() + 1;
        Chunk chunk{document.source, document.relativePath, document.title, section,
                    id.str(), "structural", text};
        chunk.hash = hashText(text);
        chunk.page = document.page;
        chunk.lineStart = firstLine;
        chunk.lineEnd = lineNumber;
        chunks.push_back(std::move(chunk));
    };
    while (std::getline(input, line)) {
        std::smatch match;
        std::string nextSection;
        if (document.type == "markdown" && std::regex_search(line, match, markdown)) nextSection = match[1];
        else if (document.type == "python" && std::regex_search(line, match, python)) nextSection = match[1].str() + " " + match[2].str();
        else if (document.type == "cpp" && std::regex_search(line, match, declaration)) nextSection = match[1].str() + " " + match[2].str();
        else if (document.type == "cpp" && std::regex_search(line, match, function)) nextSection = match[1];
        else if (document.type == "javascript" && std::regex_search(line, match, javascript)) nextSection = match[2];
        if (!nextSection.empty()) { flush(); section = nextSection; firstLine = lineNumber + 1; }
        ++lineNumber;
        buffer += line + '\n';
        // Large sections split only at paragraph/statement/block boundaries.
        // Lines and statements are never cut into character windows.
        const std::string clean = trim(line);
        const bool boundary = clean.empty() || clean.back() == ';' || clean == "}" || clean == "};";
        if (maxChunk && buffer.size() >= maxChunk && boundary) { flush(); firstLine = lineNumber + 1; }
    }
    flush();
    return chunks;
}

double DocumentIndexer::cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || a.size() != b.size()) return 0;
    double dot = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < a.size(); ++i) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
    return aa && bb ? dot / std::sqrt(aa * bb) : 0;
}
bool DocumentIndexer::validSearchRequest(const std::string& query, int topK) {
    return !trim(query).empty() && query.size() <= 16000 && topK >= 1 && topK <= 20;
}
std::vector<size_t> DocumentIndexer::rankVectors(const std::vector<float>& query,
        const std::vector<std::vector<float>>& vectors, int topK) {
    std::vector<size_t> order(vectors.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return cosineSimilarity(query, vectors[a]) > cosineSimilarity(query, vectors[b]);
    });
    if (topK >= 0 && order.size() > static_cast<size_t>(topK)) order.resize(topK);
    return order;
}
bool DocumentIndexer::embedTexts(const std::vector<std::string>& texts,
        std::vector<std::vector<float>>& vectors, std::string& error) {
    vectors.clear(); error.clear();
    for (size_t start = 0; start < texts.size(); start += kBatchSize) {
        const size_t end = std::min(start + kBatchSize, texts.size());
        std::string body = "{\"model\":\"" + escape(env("OLLAMA_EMBED_MODEL", "bge-m3")) + "\",\"input\":[";
        for (size_t i = start; i < end; ++i) { if (i > start) body += ','; body += '"' + escape(texts[i]) + '"'; }
        std::string response;
        if (!curlRequest(env("OLLAMA_URL", "http://127.0.0.1:11434") + "/api/embed", body + "]}", response, error)) return false;
        app_json::JsonValue root;
        if (!app_json::JsonParser(response).parse(root, error)) { error = "Invalid Ollama embedding response"; return false; }
        const auto* array = root.member("embeddings");
        if (!array || array->type != app_json::JsonValue::Type::Array || array->array.size() != end - start) {
            error = "Invalid Ollama embedding response"; return false;
        }
        for (const auto& item : array->array) {
            if (item.type != app_json::JsonValue::Type::Array || item.array.empty()) { error = "Invalid embedding vector"; return false; }
            std::vector<float> vector;
            for (const auto& number : item.array) {
                try {
                    if (number.type != app_json::JsonValue::Type::Number) throw std::runtime_error("number");
                    const float value = std::stof(number.text);
                    if (!std::isfinite(value)) throw std::runtime_error("finite");
                    vector.push_back(value);
                } catch (...) { error = "Invalid embedding vector"; return false; }
            }
            if (std::none_of(vector.begin(), vector.end(), [](float value) { return value != 0; })) { error = "Empty embedding vector"; return false; }
            vectors.push_back(std::move(vector));
        }
    }
    return true;
}

bool DocumentIndexer::hasIndex(std::string& error) const {
    try { auto db = openDb(dbPath_); return indexExists(db.get()); }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool DocumentIndexer::ensureReady(bool& ready, std::string& error) {
    ready = false; error.clear();
    { std::lock_guard<std::mutex> lock(mutex_); if (running_) return true; }
    ready = hasIndex(error);
    if (!error.empty()) return false;
    return ready || start(error);
}
bool DocumentIndexer::start(std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    error.clear();
    if (running_) { error = "Indexing is already running"; return false; }
    if (worker_.joinable()) worker_.join();
    phase_ = "Scanning project sources"; progress_ = progressTotal_ = 0; jobError_.clear();
    running_ = true;
    try { worker_ = std::thread(&DocumentIndexer::runJob, this); }
    catch (const std::exception&) { running_ = false; error = "Could not start indexing"; return false; }
    return true;
}
void DocumentIndexer::runJob() {
    std::string error;
    try {
        const auto started = std::chrono::steady_clock::now();
        CorpusStats corpus;
        const auto documents = loadDocuments(fs::current_path(), corpus);
        std::vector<Chunk> chunks;
        for (const auto& document : documents) {
            auto more = structuralChunks(document);
            chunks.insert(chunks.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        if (chunks.empty()) throw std::runtime_error("No project source documents found");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            corpus_ = corpus; phase_ = "Generating embeddings"; progressTotal_ = static_cast<int>(chunks.size());
        }
        std::vector<std::vector<float>> vectors;
        size_t dimension = 0;
        for (size_t i = 0; i < chunks.size(); i += kBatchSize) {
            std::vector<std::string> batch;
            for (size_t j = i; j < std::min(i + kBatchSize, chunks.size()); ++j) batch.push_back(chunks[j].content);
            std::vector<std::vector<float>> embedded;
            if (!embedTexts(batch, embedded, error)) throw std::runtime_error(error);
            for (auto& vector : embedded) {
                if (!dimension) dimension = vector.size();
                if (vector.size() != dimension) throw std::runtime_error("Embedding dimensions differ");
                vectors.push_back(std::move(vector));
            }
            std::lock_guard<std::mutex> lock(mutex_); progress_ = static_cast<int>(vectors.size());
        }
        { std::lock_guard<std::mutex> lock(mutex_); phase_ = "Saving index"; }
        auto db = openDb(dbPath_);
        exec(db.get(), "BEGIN IMMEDIATE;");
        try {
            // Replace atomically; legacy non-structural data is no longer used.
            exec(db.get(), "DELETE FROM document_chunks WHERE source_type='project'; DELETE FROM indexing_runs;");
            auto insert = prepare(db.get(), "INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at,line_start,line_end) VALUES('structural',?,?,?,?,?,?,?,?,?,?,?,?);");
            const std::string indexedAt = nowUtc();
            int totalChars = 0;
            for (size_t i = 0; i < chunks.size(); ++i) {
                const auto& chunk = chunks[i]; const auto& vector = vectors[i];
                bindText(insert.get(), 1, chunk.source); bindText(insert.get(), 2, chunk.file);
                bindText(insert.get(), 3, chunk.title); bindText(insert.get(), 4, chunk.section);
                bindText(insert.get(), 5, chunk.chunkId); bindText(insert.get(), 6, chunk.content);
                bindText(insert.get(), 7, chunk.hash);
                if (sqlite3_bind_blob(insert.get(), 8, vector.data(), static_cast<int>(vector.size() * sizeof(float)), SQLITE_TRANSIENT) != SQLITE_OK ||
                    sqlite3_bind_int(insert.get(), 9, static_cast<int>(dimension)) != SQLITE_OK) throw std::runtime_error("Could not bind embedding");
                bindText(insert.get(), 10, indexedAt);
                sqlite3_bind_int(insert.get(), 11, chunk.lineStart); sqlite3_bind_int(insert.get(), 12, chunk.lineEnd);
                done(db.get(), insert.get());
                sqlite3_reset(insert.get()); sqlite3_clear_bindings(insert.get());
                totalChars += static_cast<int>(chunk.content.size());
            }
            auto run = prepare(db.get(), "INSERT INTO indexing_runs(strategy,file_count,chunk_count,total_chars,avg_chunk_chars,embedding_dim,elapsed_ms,indexed_at,status,error) VALUES('structural',?,?,?,?,?,?,?,'done','');");
            sqlite3_bind_int(run.get(), 1, corpus.fileCount); sqlite3_bind_int(run.get(), 2, static_cast<int>(chunks.size()));
            sqlite3_bind_int(run.get(), 3, totalChars); sqlite3_bind_int(run.get(), 4, totalChars / static_cast<int>(chunks.size()));
            sqlite3_bind_int(run.get(), 5, static_cast<int>(dimension));
            sqlite3_bind_int64(run.get(), 6, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
            bindText(run.get(), 7, indexedAt); done(db.get(), run.get()); exec(db.get(), "COMMIT;");
        } catch (...) { exec(db.get(), "ROLLBACK;"); throw; }
    } catch (const std::exception& exception) { error = exception.what(); }
    catch (...) { error = "Document indexing failed"; }
    std::lock_guard<std::mutex> lock(mutex_);
    jobError_ = error; phase_ = error.empty() ? "Ready" : "Error"; running_ = false;
}
std::string DocumentIndexer::statusJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string error;
    const bool ready = !running_ && hasIndex(error);
    if (error.empty()) error = jobError_;
    return "{\"ready\":" + std::string(ready ? "true" : "false") +
        ",\"running\":" + (running_ ? "true" : "false") + ",\"phase\":\"" + escape(phase_) +
        "\",\"progress\":" + std::to_string(progress_) + ",\"total\":" + std::to_string(progressTotal_) +
        ",\"error\":\"" + escape(error) + "\"}";
}
void DocumentIndexer::resumeUploads() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (uploadsResumed_) return;
    auto db = openDb(dbPath_);
    auto pending = prepare(db.get(), "SELECT id FROM rag_uploads WHERE status IN ('uploaded','indexing') ORDER BY id;");
    while (sqlite3_step(pending.get()) == SQLITE_ROW) pendingUploads_.push_back(columnText(pending.get(), 0));
    uploadsResumed_ = true;
    if (!pendingUploads_.empty() && !uploadWorker_.joinable()) uploadWorker_ = std::thread(&DocumentIndexer::runUploads, this);
    uploadsWake_.notify_one();
}
bool DocumentIndexer::upload(const std::string& name, const std::string& bytes, std::string& id, std::string& error) {
    id.clear(); error.clear();
    if (name.empty() || name.size() > 240 || name.find_first_of("/\\") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        error = "Invalid file name"; return false;
    }
    std::string type = fs::u8path(name).extension().string();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (type != ".txt" && type != ".md" && type != ".pdf" && type != ".docx") { error = "Supported formats: TXT, MD, PDF, DOCX"; return false; }
    if (bytes.empty() || bytes.size() > 10 * 1024 * 1024) { error = "File must be between 1 byte and 10 MB"; return false; }
    try {
        resumeUploads();
        auto db = openDb(dbPath_);
        auto insert = prepare(db.get(), "INSERT INTO rag_uploads(file_name,type,data) VALUES(?,?,?);");
        bindText(insert.get(), 1, name); bindText(insert.get(), 2, type);
        if (sqlite3_bind_blob(insert.get(), 3, bytes.data(), static_cast<int>(bytes.size()), SQLITE_TRANSIENT) != SQLITE_OK) throw std::runtime_error("Could not store file");
        done(db.get(), insert.get()); id = std::to_string(sqlite3_last_insert_rowid(db.get()));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pendingUploads_.push_back(id);
            if (!uploadWorker_.joinable()) uploadWorker_ = std::thread(&DocumentIndexer::runUploads, this);
        }
        uploadsWake_.notify_one();
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
}
std::string DocumentIndexer::uploadsJson() {
    try {
        resumeUploads();
        auto db = openDb(dbPath_);
        auto rows = prepare(db.get(), "SELECT id,file_name,status,error FROM rag_uploads ORDER BY id;");
        std::string result = "{\"files\":[";
        bool first = true;
        while (sqlite3_step(rows.get()) == SQLITE_ROW) {
            if (!first) result += ','; first = false;
            result += "{\"id\":\"" + escape(columnText(rows.get(), 0)) + "\",\"name\":\"" + escape(columnText(rows.get(), 1)) +
                "\",\"status\":\"" + escape(columnText(rows.get(), 2)) + "\",\"error\":\"" + escape(columnText(rows.get(), 3)) + "\"}";
        }
        return result + "]}";
    } catch (const std::exception& exception) { return "{\"error\":\"" + escape(exception.what()) + "\"}"; }
}
void DocumentIndexer::runUploads() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        uploadsWake_.wait(lock, [&] { return uploadsStopped_ || !pendingUploads_.empty(); });
        if (uploadsStopped_) return;
        const std::string id = pendingUploads_.front(); pendingUploads_.pop_front();
        lock.unlock(); indexUpload(id); lock.lock();
    }
}
namespace {
void runExtractor(const fs::path& input, const fs::path& output) {
#ifdef _WIN32
    // All arguments are application-controlled paths; uploaded names never enter a command.
    std::wstring command = L"\"" + fs::u8path(RAG_PYTHON).wstring() + L"\" \"" + fs::u8path(RAG_EXTRACTOR).wstring() +
        L"\" \"" + input.wstring() + L"\" \"" + output.wstring() + L"\"";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Could not start local document extractor");
    CloseHandle(process.hThread);
    const auto result = WaitForSingleObject(process.hProcess, 60000);
    if (result != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, 5000); CloseHandle(process.hProcess); throw std::runtime_error("Document extraction timed out"); }
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hProcess);
    if (code) throw std::runtime_error("Local document extraction failed");
#else
    const pid_t child = fork();
    if (child < 0) throw std::runtime_error("Could not start document extractor");
    if (!child) { execl(RAG_PYTHON, RAG_PYTHON, RAG_EXTRACTOR, input.c_str(), output.c_str(), static_cast<char*>(nullptr)); _exit(1); }
    int status = 0;
    for (int i = 0; i < 600; ++i) {
        if (waitpid(child, &status, WNOHANG) == child) {
            if (!WIFEXITED(status) || WEXITSTATUS(status)) throw std::runtime_error("Document extraction failed");
            return;
        }
        usleep(100000);
    }
    kill(child, SIGKILL); waitpid(child, &status, 0); throw std::runtime_error("Document extraction timed out");
#endif
}
}
void DocumentIndexer::indexUpload(const std::string& id) {
    fs::path directory;
    std::string error;
    try {
        auto db = openDb(dbPath_);
        std::string name, type, data;
        {
            auto row = prepare(db.get(), "SELECT file_name,type,data FROM rag_uploads WHERE id=?;"); bindText(row.get(), 1, id);
            if (sqlite3_step(row.get()) != SQLITE_ROW) throw std::runtime_error("Uploaded file not found");
            name = columnText(row.get(), 0); type = columnText(row.get(), 1);
            const int bytes = sqlite3_column_bytes(row.get(), 2);
            if (bytes <= 0) throw std::runtime_error("Uploaded file is empty");
            data.assign(static_cast<const char*>(sqlite3_column_blob(row.get(), 2)), bytes);
        }
        { auto state = prepare(db.get(), "UPDATE rag_uploads SET status='indexing',error='' WHERE id=?;"); bindText(state.get(), 1, id); done(db.get(), state.get()); }
        directory = fs::temp_directory_path() / ("agent-rag-" + hashText(dbPath_ + id + nowUtc()));
        if (!fs::create_directory(directory)) throw std::runtime_error("Could not create extraction directory");
        const auto input = directory / ("document" + type), output = directory / "text.json";
        { std::ofstream file(input, std::ios::binary); file.write(data.data(), data.size()); if (!file) throw std::runtime_error("Could not save extraction input"); }
        runExtractor(input, output);
        if (fs::file_size(output) > 16 * 1024 * 1024) throw std::runtime_error("Extracted text is too large");
        std::ifstream file(output, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), {});
        app_json::JsonValue result;
        if (!app_json::JsonParser(text).parse(result, error)) throw std::runtime_error("Invalid document extraction result");
        if (const auto* failure = result.member("error")) throw std::runtime_error(failure->text);
        const auto* parts = result.member("parts");
        if (!parts || parts->type != app_json::JsonValue::Type::Array) throw std::runtime_error("No extracted document parts");
        std::vector<Chunk> chunks;
        for (const auto& part : parts->array) {
            const auto* content = part.member("text"), *kind = part.member("type"), *page = part.member("page");
            if (!content || !kind || !page) throw std::runtime_error("Invalid document metadata");
            Document document{"user_upload", "upload:" + id + ":page:" + page->text, name, kind->text, content->text, std::stoi(page->text)};
            auto more = structuralChunks(document);
            for (auto& chunk : more) {
                chunk.file = name; chunk.sourceType = "upload"; chunk.uploadId = id;
                // DOCX has no stable page/line layout without rendering the document.
                if (type == ".docx") chunk.lineStart = chunk.lineEnd = 0;
                chunks.push_back(std::move(chunk));
            }
        }
        if (chunks.empty()) throw std::runtime_error("No text chunks extracted");
        std::vector<std::string> texts;
        for (const auto& chunk : chunks) texts.push_back(chunk.content);
        std::vector<std::vector<float>> vectors;
        if (!embedTexts(texts, vectors, error)) throw std::runtime_error(error);
        const size_t dimension = vectors.front().size();
        for (const auto& vector : vectors) if (vector.size() != dimension) throw std::runtime_error("Embedding dimensions differ");
        exec(db.get(), "BEGIN IMMEDIATE;");
        try {
            auto remove = prepare(db.get(), "DELETE FROM document_chunks WHERE source_type='upload' AND upload_id=?;"); bindText(remove.get(), 1, id); done(db.get(), remove.get());
            auto insert = prepare(db.get(), "INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at,source_type,upload_id,page,line_start,line_end) VALUES('structural',?,?,?,?,?,?,?,?,?,?,'upload',?,?,?,?);");
            for (size_t i = 0; i < chunks.size(); ++i) {
                const auto& c = chunks[i]; const auto& vector = vectors[i];
                bindText(insert.get(), 1, c.source); bindText(insert.get(), 2, c.file); bindText(insert.get(), 3, c.title);
                bindText(insert.get(), 4, c.section); bindText(insert.get(), 5, c.chunkId); bindText(insert.get(), 6, c.content);
                bindText(insert.get(), 7, c.hash);
                sqlite3_bind_blob(insert.get(), 8, vector.data(), static_cast<int>(vector.size() * sizeof(float)), SQLITE_TRANSIENT);
                sqlite3_bind_int(insert.get(), 9, static_cast<int>(vector.size())); bindText(insert.get(), 10, nowUtc());
                bindText(insert.get(), 11, id); sqlite3_bind_int(insert.get(), 12, c.page);
                sqlite3_bind_int(insert.get(), 13, c.lineStart); sqlite3_bind_int(insert.get(), 14, c.lineEnd);
                done(db.get(), insert.get()); sqlite3_reset(insert.get()); sqlite3_clear_bindings(insert.get());
            }
            auto state = prepare(db.get(), "UPDATE rag_uploads SET status='indexed',error='' WHERE id=?;"); bindText(state.get(), 1, id); done(db.get(), state.get());
            exec(db.get(), "COMMIT;");
        } catch (...) { exec(db.get(), "ROLLBACK;"); throw; }
    } catch (const std::exception& exception) { error = exception.what(); }
    if (!error.empty()) {
        try { auto db = openDb(dbPath_); auto state = prepare(db.get(), "UPDATE rag_uploads SET status='error',error=? WHERE id=?;"); bindText(state.get(), 1, error); bindText(state.get(), 2, id); done(db.get(), state.get()); } catch (...) {}
    }
    if (!directory.empty()) {
        std::error_code ignored;
        for (const auto& name : {"document.txt", "document.md", "document.pdf", "document.docx", "text.json"}) fs::remove(directory / name, ignored);
        fs::remove(directory, ignored);
    }
}
bool DocumentIndexer::retrieve(const std::string& query, int topK,
        std::vector<SearchHit>& hits, std::string& error) {
    hits.clear(); error.clear();
    if (!validSearchRequest(query, topK)) { error = "RAG query or top_k is invalid"; return false; }
    std::vector<std::vector<float>> queryVectors;
    if (!embedTexts({query}, queryVectors, error)) return false;
    try {
        auto db = openDb(dbPath_);
        auto statement = prepare(db.get(), "SELECT source,file,title,section,chunk_id,content,embedding,embedding_dim,source_type,upload_id,page,line_start,line_end FROM document_chunks WHERE strategy='structural';");
        int result;
        while ((result = sqlite3_step(statement.get())) == SQLITE_ROW) {
            const int dimension = sqlite3_column_int(statement.get(), 7);
            const int bytes = sqlite3_column_bytes(statement.get(), 6);
            if (dimension <= 0 || static_cast<size_t>(dimension) != queryVectors[0].size() ||
                static_cast<size_t>(bytes) != static_cast<size_t>(dimension) * sizeof(float))
                throw std::runtime_error("Index embedding dimensions are incompatible; use Reindex");
            std::vector<float> vector(dimension);
            std::memcpy(vector.data(), sqlite3_column_blob(statement.get(), 6), bytes);
            if (std::any_of(vector.begin(), vector.end(), [](float value) { return !std::isfinite(value); }))
                throw std::runtime_error("Invalid index embedding; use Reindex");
            Chunk chunk{columnText(statement.get(), 0), columnText(statement.get(), 1), columnText(statement.get(), 2),
                        columnText(statement.get(), 3), columnText(statement.get(), 4), "structural", columnText(statement.get(), 5)};
            chunk.sourceType = columnText(statement.get(), 8); chunk.uploadId = columnText(statement.get(), 9);
            chunk.page = sqlite3_column_int(statement.get(), 10); chunk.lineStart = sqlite3_column_int(statement.get(), 11);
            chunk.lineEnd = sqlite3_column_int(statement.get(), 12);
            hits.push_back({cosineSimilarity(queryVectors[0], vector), std::move(chunk)});
        }
        if (result != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.get()));
        if (hits.empty()) throw std::runtime_error("Structural index is empty; use Reindex");
        std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
        if (hits.size() > static_cast<size_t>(topK)) hits.resize(topK);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); hits.clear(); return false; }
}
}
