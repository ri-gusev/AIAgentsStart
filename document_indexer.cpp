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
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

namespace document_index {
namespace fs=std::filesystem;
namespace {
constexpr size_t kChunkSize=1400, kOverlap=200, kBatchSize=32, kMaxFileBytes=1024*1024;
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
std::string lang(const fs::path& p) { auto e=p.extension().string(); if(e==".cpp"||e==".h"||e==".hpp"||e==".c")return "cpp"; if(e==".py")return "python"; if(e==".md")return "markdown";return "text"; }
std::string basename(const std::string& p) { return fs::path(p).filename().string(); }
size_t writeBody(char* ptr,size_t size,size_t nmemb,void* ctx){auto* s=static_cast<std::string*>(ctx);s->append(ptr,size*nmemb);return size*nmemb;}
bool curlRequest(const std::string& url,const std::string& body,std::string& response,std::string& error) {
 CURL* c=curl_easy_init(); if(!c){error="Ollama unavailable";return false;} struct curl_slist* headers=nullptr;headers=curl_slist_append(headers,"Content-Type: application/json");
 curl_easy_setopt(c,CURLOPT_URL,url.c_str());curl_easy_setopt(c,CURLOPT_HTTPHEADER,headers);curl_easy_setopt(c,CURLOPT_POST,1L);curl_easy_setopt(c,CURLOPT_POSTFIELDS,body.data());curl_easy_setopt(c,CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,writeBody);curl_easy_setopt(c,CURLOPT_WRITEDATA,&response);curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,2L);curl_easy_setopt(c,CURLOPT_TIMEOUT,90L);
 const CURLcode rc=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);curl_slist_free_all(headers);curl_easy_cleanup(c);
 if(rc!=CURLE_OK||status<200||status>=300){error= (rc==CURLE_COULDNT_CONNECT||rc==CURLE_OPERATION_TIMEDOUT)?"Ollama unavailable":"Ollama embedding request failed";return false;} return true;
}
std::vector<Document> loadDocuments(const fs::path& root, CorpusStats& stats) {
    static const std::vector<std::string> allowed={".md",".txt",".cpp",".h",".hpp",".c",".py"};
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
        if(!entry.is_regular_file()) continue;
        std::error_code sizeError;
        const auto bytes=entry.file_size(sizeError);
        if(sizeError||bytes>kMaxFileBytes) continue;
        const auto ext=entry.path().extension().string();
        if(std::find(allowed.begin(),allowed.end(),ext)==allowed.end()) continue;
        const auto relativePath=fs::path(relative);
        const auto first=relativePath.begin()==relativePath.end()?std::string{}:relativePath.begin()->string();
        const bool atRoot=relativePath.parent_path().empty();
        const bool coreCpp=atRoot&&(ext==".cpp"||ext==".h"||ext==".hpp"||ext==".c");
        const bool coreReadme=(relative=="README.md"||((first=="mcp_server"||first=="codeforces_mcp_server")&&entry.path().filename()=="README.md"));
        const bool mcpPython=(first=="mcp_server"||first=="codeforces_mcp_server")&&ext==".py";
        if(!coreCpp&&!coreReadme&&!mcpPython) continue;
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
}std::string chunkJson(const Chunk& c,double score) { std::ostringstream o;o<<"{\"score\":"<<std::fixed<<std::setprecision(5)<<score<<",\"source\":\""<<escape(c.source)<<"\",\"file\":\""<<escape(c.file)<<"\",\"title\":\""<<escape(c.title)<<"\",\"section\":\""<<escape(c.section)<<"\",\"chunk_id\":\""<<escape(c.chunkId)<<"\",\"strategy\":\""<<escape(c.strategy)<<"\",\"content\":\""<<escape(c.content)<<"\"}";return o.str(); }
bool probeOllama() { CURL* c=curl_easy_init(); if(!c)return false; std::string response; const std::string url=env("OLLAMA_URL","http://127.0.0.1:11434")+"/api/tags"; curl_easy_setopt(c,CURLOPT_URL,url.c_str()); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,writeBody); curl_easy_setopt(c,CURLOPT_WRITEDATA,&response); curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,1L); curl_easy_setopt(c,CURLOPT_TIMEOUT,2L); long code=0; const auto rc=curl_easy_perform(c); curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code); curl_easy_cleanup(c); return rc==CURLE_OK&&code>=200&&code<300; }


bool dbOpen(const std::string& path,sqlite3*& db,std::string& error){if(sqlite3_open(path.c_str(),&db)!=SQLITE_OK){error="Could not open document index database";return false;}const char* sql="PRAGMA journal_mode=WAL; CREATE TABLE IF NOT EXISTS document_chunks(id INTEGER PRIMARY KEY,strategy TEXT NOT NULL,source TEXT NOT NULL,file TEXT NOT NULL,title TEXT NOT NULL,section TEXT NOT NULL,chunk_id TEXT NOT NULL,content TEXT NOT NULL,content_hash TEXT NOT NULL,embedding BLOB NOT NULL,embedding_dim INTEGER NOT NULL,indexed_at TEXT NOT NULL,UNIQUE(strategy,chunk_id)); CREATE TABLE IF NOT EXISTS indexing_runs(strategy TEXT PRIMARY KEY,file_count INTEGER,chunk_count INTEGER,total_chars INTEGER,avg_chunk_chars INTEGER,embedding_dim INTEGER,elapsed_ms INTEGER,indexed_at TEXT,status TEXT,error TEXT);";char* msg=nullptr;if(sqlite3_exec(db,sql,nullptr,nullptr,&msg)!=SQLITE_OK){error=msg?msg:"SQLite schema error";sqlite3_free(msg);sqlite3_close(db);return false;}return true;}
std::string runStatsJson(sqlite3* db,const std::string& strategy){sqlite3_stmt* s=nullptr;std::string out="null";if(sqlite3_prepare_v2(db,"SELECT file_count,chunk_count,total_chars,avg_chunk_chars,embedding_dim,elapsed_ms,indexed_at,status,error FROM indexing_runs WHERE strategy=?",-1,&s,nullptr)==SQLITE_OK){sqlite3_bind_text(s,1,strategy.c_str(),-1,SQLITE_TRANSIENT);if(sqlite3_step(s)==SQLITE_ROW){auto txt=[&](int i){auto p=sqlite3_column_text(s,i);return p?std::string(reinterpret_cast<const char*>(p)):std::string{};};out="{\"file_count\":"+std::to_string(sqlite3_column_int(s,0))+",\"chunk_count\":"+std::to_string(sqlite3_column_int(s,1))+",\"total_chars\":"+std::to_string(sqlite3_column_int(s,2))+",\"avg_chunk_chars\":"+std::to_string(sqlite3_column_int(s,3))+",\"embedding_dim\":"+std::to_string(sqlite3_column_int(s,4))+",\"elapsed_ms\":"+std::to_string(sqlite3_column_int64(s,5))+",\"indexed_at\":\""+escape(txt(6))+"\",\"status\":\""+escape(txt(7))+"\",\"error\":\""+escape(txt(8))+"\"}";}}sqlite3_finalize(s);return out;}
std::vector<SearchHit> searchDb(sqlite3* db,const std::string& strategy,const std::vector<float>& q,int topK){std::vector<SearchHit> hits;std::vector<std::vector<float>> embeddings;sqlite3_stmt* s=nullptr;if(sqlite3_prepare_v2(db,"SELECT source,file,title,section,chunk_id,content,embedding,embedding_dim FROM document_chunks WHERE strategy=?",-1,&s,nullptr)!=SQLITE_OK)return hits;sqlite3_bind_text(s,1,strategy.c_str(),-1,SQLITE_TRANSIENT);while(sqlite3_step(s)==SQLITE_ROW){int dim=sqlite3_column_int(s,7);const auto* blob=static_cast<const float*>(sqlite3_column_blob(s,6));if(dim<=0||!blob||sqlite3_column_bytes(s,6)!=dim*static_cast<int>(sizeof(float)))continue;std::vector<float> v(blob,blob+dim);Chunk c;c.strategy=strategy;c.source=reinterpret_cast<const char*>(sqlite3_column_text(s,0));c.file=reinterpret_cast<const char*>(sqlite3_column_text(s,1));c.title=reinterpret_cast<const char*>(sqlite3_column_text(s,2));c.section=reinterpret_cast<const char*>(sqlite3_column_text(s,3));c.chunkId=reinterpret_cast<const char*>(sqlite3_column_text(s,4));c.content=reinterpret_cast<const char*>(sqlite3_column_text(s,5));hits.push_back({DocumentIndexer::cosineSimilarity(q,v),std::move(c)});embeddings.push_back(std::move(v));}sqlite3_finalize(s);const auto ranked=DocumentIndexer::rankVectors(q,embeddings,topK);std::vector<SearchHit> sorted;for(const auto i:ranked)sorted.push_back(std::move(hits[i]));return sorted;}
std::string hitsJson(const std::vector<SearchHit>& hits){std::string r="[";for(size_t i=0;i<hits.size();++i){if(i)r+=",";r+=chunkJson(hits[i].chunk,hits[i].score);}return r+"]";}
}
DocumentIndexer::DocumentIndexer():dbPath_(databasePath()) { sqlite3* db=nullptr;std::string e;if(dbOpen(dbPath_,db,e))sqlite3_close(db); }
DocumentIndexer::~DocumentIndexer(){if(worker_.joinable())worker_.join();}
std::string DocumentIndexer::databasePath(){return env("DOCUMENT_INDEX_DB","document_index.db");}
CorpusStats DocumentIndexer::inspectCorpus(const std::string& root){CorpusStats stats;auto documents=loadDocuments(root,stats);(void)documents;return stats;}
std::vector<Chunk> DocumentIndexer::fixedChunks(const Document& d,size_t size,size_t overlap){std::vector<Chunk> out;if(size==0)return out;overlap=std::min(overlap,size-1);size_t pos=0;int id=1;while(pos<d.content.size()){size_t end=std::min(pos+size,d.content.size());size_t left=pos;while(left<end&&std::isspace(static_cast<unsigned char>(d.content[left])))++left;size_t right=end;while(right>left&&std::isspace(static_cast<unsigned char>(d.content[right-1])))--right;if(right>left){Chunk c{d.source,d.relativePath,d.title,"",d.relativePath+":fixed:"+([&]{std::ostringstream s;s<<std::setw(4)<<std::setfill('0')<<id++;return s.str();})(),"fixed",d.content.substr(left,right-left)};c.hash=hashText(c.content);out.push_back(std::move(c));}if(end==d.content.size())break;pos=end-overlap;}return out;}
std::vector<Chunk> DocumentIndexer::structuralChunks(const Document& d,size_t maxChunk){std::vector<Chunk> out;std::vector<std::pair<std::string,std::string>> sections;std::string current="",buffer;std::istringstream in(d.content);std::string line;std::regex md(R"(^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$)"), py(R"(^\s*(async\s+def|def|class)\s+([A-Za-z_][A-Za-z0-9_]*))"), cpp(R"(^\s*(namespace|class|struct)\s+([A-Za-z_][A-Za-z0-9_:]*))"), func(R"(^\s*(?:[\w:<>&*]+\s+)+([A-Za-z_~][\w:~]*)\s*\([^;]*\)\s*(?:const\s*)?(?:\{|$))");
 while(std::getline(in,line)){std::smatch m;std::string next;if(d.type=="markdown"&&std::regex_search(line,m,md))next=trim(m[1]);else if(d.type=="python"&&std::regex_search(line,m,py))next=m[1].str()+" "+m[2].str();else if(d.type=="cpp"){if(std::regex_search(line,m,cpp))next=m[1].str()+" "+m[2].str();else if(std::regex_search(line,m,func))next=m[1].str();}if(!next.empty()&&!buffer.empty()){sections.emplace_back(current,std::move(buffer));buffer.clear();}if(!next.empty())current=next;buffer+=line+"\n";}if(!buffer.empty())sections.emplace_back(current,std::move(buffer));
 int id=1;for(auto& sec:sections){Document part=d;part.content=std::move(sec.second);if(part.content.size()<=maxChunk){Chunk c{d.source,d.relativePath,d.title,sec.first,d.relativePath+":structural:"+([&]{std::ostringstream s;s<<std::setw(4)<<std::setfill('0')<<id++;return s.str();})(),"structural",trim(part.content)};c.hash=hashText(c.content);if(!c.content.empty())out.push_back(std::move(c));}else for(auto c:fixedChunks(part,maxChunk,150)){c.strategy="structural";c.section=sec.first;c.chunkId=d.relativePath+":structural:"+([&]{std::ostringstream s;s<<std::setw(4)<<std::setfill('0')<<id++;return s.str();})();c.hash=hashText(c.content);out.push_back(std::move(c));}}
 return out;}
double DocumentIndexer::cosineSimilarity(const std::vector<float>&a,const std::vector<float>&b){if(a.empty()||a.size()!=b.size())return 0;double dot=0,aa=0,bb=0;for(size_t i=0;i<a.size();++i){dot+=a[i]*b[i];aa+=a[i]*a[i];bb+=b[i]*b[i];}return aa&&bb?dot/std::sqrt(aa*bb):0;}bool DocumentIndexer::validIndexStrategy(const std::string& strategy){return strategy=="fixed"||strategy=="structural"||strategy=="both";}
bool DocumentIndexer::validSearchRequest(const std::string& query,const std::string& strategy,int topK){return !query.empty()&&query.size()<=4000&&(strategy=="fixed"||strategy=="structural")&&topK>=1&&topK<=20;}
std::vector<size_t> DocumentIndexer::rankVectors(const std::vector<float>& query,const std::vector<std::vector<float>>& vectors,int topK){std::vector<size_t> order(vectors.size());for(size_t i=0;i<order.size();++i)order[i]=i;std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){return cosineSimilarity(query,vectors[a])>cosineSimilarity(query,vectors[b]);});if(topK>=0&&order.size()>static_cast<size_t>(topK))order.resize(static_cast<size_t>(topK));return order;}
bool DocumentIndexer::embedTexts(const std::vector<std::string>& texts,std::vector<std::vector<float>>& vectors,std::string& error){vectors.clear();for(size_t start=0;start<texts.size();start+=kBatchSize){const size_t end=std::min(start+kBatchSize,texts.size());std::string body="{\"model\":\""+escape(env("OLLAMA_EMBED_MODEL","bge-m3"))+"\",\"input\":[";for(size_t i=start;i<end;++i){if(i>start)body+=",";body+="\""+escape(texts[i])+"\"";}body+="]}";std::string response;if(!curlRequest(env("OLLAMA_URL","http://127.0.0.1:11434")+"/api/embed",body,response,error))return false;app_json::JsonValue root;if(!app_json::JsonParser(response).parse(root,error)){error="Invalid Ollama embedding response";return false;}auto* arr=root.member("embeddings");if(!arr||arr->type!=app_json::JsonValue::Type::Array||arr->array.size()!=end-start){error="Invalid Ollama embedding response";return false;}for(const auto& a:arr->array){if(a.type!=app_json::JsonValue::Type::Array){error="Invalid Ollama embedding response";return false;}std::vector<float> v;for(const auto& x:a.array){try{v.push_back(std::stof(x.text));}catch(...){v.clear();break;}}if(v.empty()){error="Invalid Ollama embedding response";return false;}vectors.push_back(std::move(v));}}return true;}
bool DocumentIndexer::start(const std::string& strategy,std::string& error){if(!validIndexStrategy(strategy)){error="strategy must be fixed, structural, or both";return false;}std::lock_guard<std::mutex> lock(mutex_);if(running_){error="Indexing is already running";return false;}if(worker_.joinable())worker_.join();running_=true;phase_="Loading files";progress_=0;progressTotal_=0;jobError_.clear();worker_=std::thread(&DocumentIndexer::runJob,this,strategy);return true;}
void DocumentIndexer::runJob(std::string strategy){CorpusStats cs;std::string error;auto docs=loadDocuments(fs::current_path(),cs);{std::lock_guard<std::mutex> l(mutex_);corpus_=cs;phase_="Chunking";}std::vector<std::string> strategies=strategy=="both"?std::vector<std::string>{"fixed","structural"}:std::vector<std::string>{strategy};
 for(const auto& st:strategies){const auto strategyStarted=std::chrono::steady_clock::now();std::vector<Chunk> chunks;for(const auto& d:docs){auto more=st=="fixed"?fixedChunks(d):structuralChunks(d);chunks.insert(chunks.end(),std::make_move_iterator(more.begin()),std::make_move_iterator(more.end()));}RunStats stats;stats.fileCount=cs.fileCount;stats.chunkCount=static_cast<int>(chunks.size());for(auto& c:chunks)stats.totalChars+=static_cast<int>(c.content.size());stats.avgChunkChars=chunks.empty()?0:stats.totalChars/static_cast<int>(chunks.size());{std::lock_guard<std::mutex> l(mutex_);progress_=0;progressTotal_=stats.chunkCount;phase_="Generating embeddings";}
  std::vector<std::vector<float>> vectors;for(size_t i=0;i<chunks.size();i+=kBatchSize){std::vector<std::string> batch;const auto end=std::min(i+kBatchSize,chunks.size());for(size_t j=i;j<end;++j)batch.push_back(chunks[j].content);std::vector<std::vector<float>> result;if(!embedTexts(batch,result,error))break;for(auto& v:result){if(!stats.embeddingDim)stats.embeddingDim=static_cast<int>(v.size());if(static_cast<int>(v.size())!=stats.embeddingDim){error="Embedding dimensions differ";break;}vectors.push_back(std::move(v));}{std::lock_guard<std::mutex> l(mutex_);progress_=static_cast<int>(vectors.size());}}
  if(!error.empty())break;{std::lock_guard<std::mutex> l(mutex_);phase_="Saving SQLite";}sqlite3* db=nullptr;if(!dbOpen(dbPath_,db,error))break;sqlite3_exec(db,"BEGIN IMMEDIATE;",nullptr,nullptr,nullptr);sqlite3_stmt* del=nullptr;sqlite3_prepare_v2(db,"DELETE FROM document_chunks WHERE strategy=?",-1,&del,nullptr);sqlite3_bind_text(del,1,st.c_str(),-1,SQLITE_TRANSIENT);sqlite3_step(del);sqlite3_finalize(del);sqlite3_stmt* ins=nullptr;sqlite3_prepare_v2(db,"INSERT INTO document_chunks(strategy,source,file,title,section,chunk_id,content,content_hash,embedding,embedding_dim,indexed_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)",-1,&ins,nullptr);const std::string indexed=nowUtc();for(size_t i=0;i<chunks.size();++i){auto& c=chunks[i];auto& v=vectors[i];sqlite3_bind_text(ins,1,st.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,2,c.source.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,3,c.file.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,4,c.title.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,5,c.section.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,6,c.chunkId.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(ins,7,c.content.c_str(),static_cast<int>(c.content.size()),SQLITE_TRANSIENT);sqlite3_bind_text(ins,8,c.hash.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_blob(ins,9,v.data(),static_cast<int>(v.size()*sizeof(float)),SQLITE_TRANSIENT);sqlite3_bind_int(ins,10,static_cast<int>(v.size()));sqlite3_bind_text(ins,11,indexed.c_str(),-1,SQLITE_TRANSIENT);sqlite3_step(ins);sqlite3_reset(ins);sqlite3_clear_bindings(ins);}sqlite3_finalize(ins);sqlite3_exec(db,"COMMIT;",nullptr,nullptr,nullptr);stats.elapsedMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-strategyStarted).count();stats.indexedAt=indexed;stats.status="done";sqlite3_stmt* run=nullptr;sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO indexing_runs VALUES(?,?,?,?,?,?,?,?,?,?)",-1,&run,nullptr);sqlite3_bind_text(run,1,st.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int(run,2,stats.fileCount);sqlite3_bind_int(run,3,stats.chunkCount);sqlite3_bind_int(run,4,stats.totalChars);sqlite3_bind_int(run,5,stats.avgChunkChars);sqlite3_bind_int(run,6,stats.embeddingDim);sqlite3_bind_int64(run,7,stats.elapsedMs);sqlite3_bind_text(run,8,indexed.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(run,9,"done",-1,SQLITE_STATIC);sqlite3_bind_text(run,10,"",-1,SQLITE_STATIC);sqlite3_step(run);sqlite3_finalize(run);sqlite3_close(db);
 }
 if(!error.empty()){sqlite3* db=nullptr;if(dbOpen(dbPath_,db,error)){for(const auto& st:strategies){sqlite3_stmt* s=nullptr;sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO indexing_runs(strategy,status,error,indexed_at) VALUES(?,?,?,?)",-1,&s,nullptr);sqlite3_bind_text(s,1,st.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,"error",-1,SQLITE_STATIC);sqlite3_bind_text(s,3,error.c_str(),-1,SQLITE_TRANSIENT);auto t=nowUtc();sqlite3_bind_text(s,4,t.c_str(),-1,SQLITE_TRANSIENT);sqlite3_step(s);sqlite3_finalize(s);}sqlite3_close(db);}}
 {std::lock_guard<std::mutex> l(mutex_);jobError_=error;phase_=error.empty()?"Done":"Error";running_=false;}}
std::string DocumentIndexer::statusJson(){CorpusStats cs;bool running;std::string phase,err;int p,total;{std::lock_guard<std::mutex> l(mutex_);cs=corpus_;running=running_;phase=phase_;err=jobError_;p=progress_;total=progressTotal_;}if(cs.fileCount==0){auto d=loadDocuments(fs::current_path(),cs);(void)d;std::lock_guard<std::mutex> l(mutex_);corpus_=cs;}bool online=probeOllama();sqlite3* db=nullptr;std::string e;std::string fixed="null",structural="null";if(dbOpen(dbPath_,db,e)){fixed=runStatsJson(db,"fixed");structural=runStatsJson(db,"structural");sqlite3_close(db);}return "{\"ollama\":{\"online\":"+std::string(online?"true":"false")+",\"error\":\""+escape(online?"":"Ollama unavailable")+"\"},\"model\":\""+escape(env("OLLAMA_EMBED_MODEL","bge-m3"))+"\",\"database\":\""+escape(dbPath_)+"\",\"corpus\":{\"file_count\":"+std::to_string(cs.fileCount)+",\"total_lines\":"+std::to_string(cs.totalLines)+",\"total_characters\":"+std::to_string(cs.totalCharacters)+",\"equivalent_pages_approx\":"+std::to_string(cs.totalCharacters/3000)+"},\"fixed\":"+fixed+",\"structural\":"+structural+",\"job\":{\"running\":"+std::string(running?"true":"false")+",\"phase\":\""+escape(phase)+"\",\"progress\":"+std::to_string(p)+",\"total\":"+std::to_string(total)+",\"error\":\""+escape(err)+"\"}}";}
std::string DocumentIndexer::searchJson(const std::string& query,const std::string& strategy,int topK){if(!validSearchRequest(query,strategy,topK))return "{\"error\":\"query, strategy, or top_k is invalid\"}";std::vector<std::vector<float>> v;std::string e;if(!embedTexts({query},v,e))return "{\"error\":\""+escape(e)+"\"}";sqlite3* db=nullptr;if(!dbOpen(dbPath_,db,e))return "{\"error\":\""+escape(e)+"\"}";auto hits=searchDb(db,strategy,v[0],topK);sqlite3_close(db);return "{\"strategy\":\""+strategy+"\",\"query\":\""+escape(query)+"\",\"results\":"+hitsJson(hits)+"}";}
std::string DocumentIndexer::compareJson(const std::string& query,int topK){if(query.empty()||query.size()>4000||topK<1||topK>20)return "{\"error\":\"query or top_k is invalid\"}";std::vector<std::vector<float>> v;std::string e;if(!embedTexts({query},v,e))return "{\"error\":\""+escape(e)+"\"}";sqlite3* db=nullptr;if(!dbOpen(dbPath_,db,e))return "{\"error\":\""+escape(e)+"\"}";auto f=searchDb(db,"fixed",v[0],topK),s=searchDb(db,"structural",v[0],topK);auto fj=runStatsJson(db,"fixed"),sj=runStatsJson(db,"structural");sqlite3_close(db);return "{\"query\":\""+escape(query)+"\",\"fixed\":{\"stats\":"+fj+",\"results\":"+hitsJson(f)+"},\"structural\":{\"stats\":"+sj+",\"results\":"+hitsJson(s)+"}}";}
}
