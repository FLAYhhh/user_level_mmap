cmake_minimum_required(VERSION 3.16)
project(user_level_mmap C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(PTEditor)
add_subdirectory(src)

add_subdirectory(demos)

