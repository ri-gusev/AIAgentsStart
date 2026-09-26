cmake_minimum_required(VERSION 3.15)
project(openai_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# CMake 4 exposes the modern SQLite3::SQLite3 target, while older supported
# CMake releases used SQLite::SQLite3.
if(TARGET SQLite3::SQLite3)
    set(SQLITE_TARGET SQLite3::SQLite3)
else()
    set(SQLITE_TARGET SQLite::SQLite3)
endif()

set(REMINDER_SOURCES reminder_store.cpp reminder_scheduler.cpp reminder_events.cpp)
add_executable(openai_cli main.cpp agent.cpp api_client.cpp web_server.cpp memory_store.cpp invariant_store.cpp mcp_client.cpp ${REMINDER_SOURCES})
target_link_libraries(openai_cli PRIVATE CURL::libcurl ${SQLITE_TARGET} Threads::Threads)

if(WIN32)
    target_link_libraries(openai_cli PRIVATE ws2_32 bcrypt crypt32)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(reminder_tests tests/reminder_tests.cpp reminder_store.cpp reminder_scheduler.cpp)
    target_include_directories(reminder_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(reminder_tests PRIVATE PROJECT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(reminder_tests PRIVATE ${SQLITE_TARGET} Threads::Threads)
    add_test(NAME reminder_persistence_and_scheduler COMMAND reminder_tests)
    # Test transport is implemented in the test file; no paid API calls.
    add_executable(agent_tests tests/agent_tests.cpp agent.cpp memory_store.cpp invariant_store.cpp)
    target_include_directories(agent_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(agent_tests PRIVATE ${SQLITE_TARGET})
    add_test(NAME agent_memory_and_policies COMMAND agent_tests)

    add_executable(web_parser_tests tests/web_parser_tests.cpp agent.cpp api_client.cpp memory_store.cpp invariant_store.cpp mcp_client.cpp ${REMINDER_SOURCES})
    target_include_directories(web_parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(web_parser_tests PRIVATE CURL::libcurl ${SQLITE_TARGET} Threads::Threads)
    if(WIN32)
        target_link_libraries(web_parser_tests PRIVATE ws2_32 bcrypt crypt32)
    endif()
    add_test(NAME http_request_parser COMMAND web_parser_tests)

    add_executable(web_transport_tests tests/web_transport_tests.cpp agent.cpp api_client.cpp memory_store.cpp invariant_store.cpp mcp_client.cpp ${REMINDER_SOURCES})
    target_include_directories(web_transport_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(web_transport_tests PRIVATE CURL::libcurl ${SQLITE_TARGET} Threads::Threads)
    if(WIN32)
        target_link_libraries(web_transport_tests PRIVATE ws2_32 bcrypt crypt32)
    endif()
    add_test(NAME tcp_http_and_websocket_transport COMMAND web_transport_tests)

    add_executable(mcp_client_tests tests/mcp_client_tests.cpp)
    target_include_directories(mcp_client_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mcp_client_tests PRIVATE CURL::libcurl)
    if(WIN32)
        target_link_libraries(mcp_client_tests PRIVATE ws2_32)
    endif()
    add_test(NAME mcp_response_parser COMMAND mcp_client_tests)

    add_executable(memory_store_tests tests/memory_store_tests.cpp memory_store.cpp)
    target_include_directories(memory_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(memory_store_tests PRIVATE ${SQLITE_TARGET})
    add_test(NAME task_state_storage COMMAND memory_store_tests)
endif()
