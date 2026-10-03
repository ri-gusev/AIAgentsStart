# AI Agent

A local C++17 web application for AI chat, project task workflows, memory, MCP tools, reminders, and semantic search over the project source and documentation.

## Architecture

The C++ backend serves two chat modes: Tasks uses the task state machine; Assistant handles conversation and MCP tools. Chat IDs in the existing memory database isolate working memory and conversation summaries while sharing long-term facts. Document Index uses its own SQLite database and local Ollama embeddings; indexed documents are not sent to OpenAI.

## Stack

C++17, CMake, libcurl, SQLite, HTML, CSS, JavaScript, Ollama `/api/embed` (`bge-m3` by default).
