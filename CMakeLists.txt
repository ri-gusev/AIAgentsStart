cmake_minimum_required(VERSION 3.15)
project(openai_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

# CMake 4 exposes the modern SQLite3::SQLite3 target, while older supported
# CMake releases used SQLite::SQLite3.
if(TARGET SQLite3::SQLite3)
    set(SQLITE_TARGET SQLite3::SQLite3)
else()
    set(SQLITE_TARGET SQLite::SQLite3)
endif()

add_executable(openai_cli main.cpp agent.cpp api_client.cpp web_server.cpp memory_store.cpp invariant_store.cpp)
target_link_libraries(openai_cli PRIVATE CURL::libcurl ${SQLITE_TARGET})

if(WIN32)
    target_link_libraries(openai_cli PRIVATE ws2_32)
endif()

include(CTest)
if(BUILD_TESTING)
    # Test transport is implemented in the test file; no paid API calls.
    add_executable(agent_tests tests/agent_tests.cpp agent.cpp memory_store.cpp invariant_store.cpp)
    target_include_directories(agent_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(agent_tests PRIVATE ${SQLITE_TARGET})
    add_test(NAME agent_memory_and_policies COMMAND agent_tests)

    add_executable(web_parser_tests tests/web_parser_tests.cpp agent.cpp api_client.cpp memory_store.cpp invariant_store.cpp)
    target_include_directories(web_parser_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(web_parser_tests PRIVATE CURL::libcurl ${SQLITE_TARGET})
    if(WIN32)
        target_link_libraries(web_parser_tests PRIVATE ws2_32)
    endif()
    add_test(NAME http_request_parser COMMAND web_parser_tests)

    add_executable(memory_store_tests tests/memory_store_tests.cpp memory_store.cpp)
    target_include_directories(memory_store_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(memory_store_tests PRIVATE ${SQLITE_TARGET})
    add_test(NAME task_state_storage COMMAND memory_store_tests)
endif()
