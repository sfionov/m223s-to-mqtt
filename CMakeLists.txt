cmake_minimum_required(VERSION 3.17)
project(m223s)

set(CMAKE_CXX_STANDARD 17)

link_libraries(systemd mosquitto expat)
add_executable(m223s main.cpp)