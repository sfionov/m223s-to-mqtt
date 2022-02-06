cmake_minimum_required(VERSION 3.17)
project(m223s)

set(CMAKE_CXX_STANDARD 17)

link_libraries(systemd mosquitto expat)
include_directories(third-party)
add_compile_definitions(FMT_HEADER_ONLY=1)
add_executable(m223s main.cpp)
