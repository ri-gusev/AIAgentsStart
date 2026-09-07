#include "agent.h"
#include "web_server.h"

#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int main() {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    Agent agent;
    if (!agent.isReady()) {
        std::cerr << agent.initializationError() << '\n';
        return 1;
    }
    return runWebServer(agent);
}
