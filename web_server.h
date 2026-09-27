#pragma once

class Agent;
class McpClient;
class McpManager;

int runWebServer(Agent& agent, McpClient& mcpClient, McpManager* manager = nullptr);
