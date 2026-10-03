# AI Agent

A local C++17 web application for AI chat, project task workflows, memory, MCP tools, reminders, and optional RAG over project sources.

## Architecture

The C++ backend serves two chat modes: Tasks uses the task state machine; Assistant handles conversation and MCP tools. Chat IDs isolate working memory and summaries while sharing long-term facts. RAG retrieves text from a structural index in `document_index.db`, built locally with Ollama when missing or manually reindexed. Retrieved excerpts and metadata go to the existing LLM; embedding vectors stay local.

## Stack

C++17, CMake, libcurl, SQLite, HTML, CSS, JavaScript, Ollama `/api/embed` (`bge-m3` by default).
