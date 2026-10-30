# libmytvhook.so has to link ezinject's CRT objects from the same ezinject build that injects it
# (the CRT is the entry point ezinject calls), so it is built with ezinject's own CMake helper.
cmake_minimum_required(VERSION 3.13)
project(mytvhook C)

find_package(ezinject REQUIRED)

add_ezinject_library(mytvhook SOURCES mytvhook.c)
target_link_libraries(mytvhook dl)
target_compile_options(mytvhook PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter)
target_link_options(mytvhook PRIVATE -Wl,-s)
