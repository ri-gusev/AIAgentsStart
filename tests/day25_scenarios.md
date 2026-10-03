# Day 25 conversation scenarios

Both automated scenarios are in `rag_pipeline_tests.cpp`. They use temporary SQLite databases,
the actual DocumentIndexer HTTP transport with a local deterministic embedding stub, and scripted
LLM responses. They verify context assembly and state persistence; they do not measure a real
model's ability to resolve references or judge the semantic relevance of real bge-m3 embeddings.

| Turn | Scenario 1: architecture | Scenario 2: uploaded PDF and code | Expected working state |
| --- | --- | --- | --- |
| 1 | Set the RAG chat goal | Review architecture.pdf and mcp_manager.cpp | Save goal |
| 2 | Name option 1 SQLite cache, option 2 file cache | Compare those options in the guide | Save option mapping |
| 3 | Use SQLite and structural chunking | Same initial requirements | Save constraints |
| 4 | Keep existing Agent and chat | Same architecture boundary | Add constraint; summarize older history |
| 5 | Define BatchUploader | Use the guide's component name | Save terminology |
| 6 | Why is the second option better? | How does that component interact with callTool? | Use summary, recent turns and working state in rewrite |
| 7 | Change to no SQLite | Change the document's default requirement to no SQLite | Replace old constraint; delete legacy task.stack |
| 8 | Choose that second option, as before | Explain it using the uploaded guide | Update clarification; summarize |
| 9 | Refine the production RAG chat goal | Same goal refinement | Replace goal |
| 10 | Ask about unresolved cache format | Ask about unrelated subject; no matching chunks | Save open question only in scenario 1; empty Sources in scenario 2 |
| 11 | Choose JSON, resolve the question | Same decision with RAG OFF | Clear open question; no retrieval when OFF |
| 12 | Implement the second option without SQLite | Resume RAG, use those sources with current constraints | Retain updated state; summarize |

Checks on every RAG turn: contextual rewrite, actual indexed project/upload chunks, threshold,
text-only final context, unchanged user question, HTTP Sources metadata, page/line information.
Scenario 1 performs 12 retrievals; scenario 2 performs 11 and one ordinary request.
Each scenario updates the existing summary three times, retains six raw messages,
reloads working state from the same SQLite database and verifies isolation from the other chat.

Run the `rag_pipeline_tests` target with CTest. Separate memory-store tests verify transactional
rollback, removal of superseded working records, and preservation of global facts and other chats.
