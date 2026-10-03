# AI Agent

A local C++17 web application for AI chat, project task workflows, memory, MCP tools, reminders, and optional RAG over project sources.

## Architecture

The C++ backend serves two chat modes: Tasks uses the task state machine; Assistant handles conversation and MCP tools. Chat IDs isolate working memory and summaries while sharing long-term facts. RAG rewrites the search query, retrieves structural chunks from `document_index.db`, filters cosine similarity and heuristically reranks candidates using `rag_config.json`. The existing LLM receives selected text, source metadata and the original question; embeddings stay local. Ollama builds missing indexes; manual reindexing refreshes project sources.

## Stack

C++17, CMake, libcurl, SQLite, HTML, CSS, JavaScript, Ollama `/api/embed` (`bge-m3`), Python + pypdf for local TXT/MD/PDF/DOCX extraction. Uploaded files and their status live in `document_index.db`; project reindexing preserves them. PDF requires a text layer (no OCR).
