# Day 24 controls

`day24_control_questions.json` contains 10 cases tied to the existing structural index snapshot:
596 chunks (552 project, 44 upload). It records expected facts and source metadata/IDs.
The report `day24_control_results.json` records the actual answer, actual sources/quotes,
retained retrieval candidates, backend refusal flag and automatic checks for each question.

## What was exercised

- Real `DocumentIndexer::retrieve`, local Ollama `bge-m3`, configured top-10/threshold/reranking/top-4.
- Existing Agent answer/review/memory flow with a scripted LLM transport.
- Backend validation of source IDs, metadata and exact quote membership in retrieved text.
- Seven supported answers, including an uploaded PDF and one answer using two code chunks.
- Two questions with zero retained chunks: the backend generates the refusal, no answer LLM call.
- One future-winner question with topical code candidates: insufficient-context response produces a refusal with no citations.

**10/10 checks passed.** This is a deterministic integration test, not a live OpenAI quality evaluation.
The scripted answers and rewrites do not prove that a real model chooses the correct facts or query.
`OPENAI_API_KEY` was unavailable in the executing environment; no live OpenAI requests were made.
Ollama was already installed; no model download or installation was performed.

## Repeat

Build `rag_pipeline_tests` with `BUILD_TESTING=ON`, then from the project root:

```powershell
& .\build\rag-day23\rag_pipeline_tests.exe --control tests/day24_control_questions.json tests/day24_control_results.json
```

The control run uses temporary memory databases; it does not save test turns into the user's chat.
After project reindexing, refresh the pinned expected source IDs/facts against the new index snapshot.
Regular CTest tests use a local embedding stub and do not require a running Ollama or API key.
