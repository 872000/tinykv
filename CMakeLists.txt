cmake_minimum_required(VERSION 3.16)
project(tinykv VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

# Core library: hand-rolled hash table, store, protocol, command dispatch.
add_library(tinykv_lib
    src/hashmap.cpp
    src/kvstore.cpp
    src/protocol.cpp
    src/commands.cpp
)
target_include_directories(tinykv_lib PUBLIC src)

find_package(Threads REQUIRED)

add_executable(tinykv-server src/server_main.cpp)
target_link_libraries(tinykv-server PRIVATE tinykv_lib Threads::Threads)

add_executable(tinykv-cli src/client_main.cpp)

# Self-contained test runner (no external framework).
add_executable(tinykv_tests tests/test_main.cpp)
target_link_libraries(tinykv_tests PRIVATE tinykv_lib)

enable_testing()
add_test(NAME tinykv_tests COMMAND tinykv_tests)
