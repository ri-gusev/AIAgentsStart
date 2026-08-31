cmake_minimum_required(VERSION 3.15)
project(openai_cli LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
add_executable(openai_cli main.cpp)
target_link_libraries(openai_cli PRIVATE CURL::libcurl)
