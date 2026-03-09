cmake_minimum_required(VERSION 3.16)
project(dnsblast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(dnsblast
    src/main.cpp
    src/dns_encode.cpp
    src/sender.cpp
    src/receiver.cpp
)

target_include_directories(dnsblast PRIVATE src)
target_compile_definitions(dnsblast PRIVATE _GNU_SOURCE)
target_link_libraries(dnsblast PRIVATE pthread)

# Optimize for the build machine
target_compile_options(dnsblast PRIVATE
    -O2
    -Wall
    -Wextra
    -march=native
)
