cmake_minimum_required(VERSION 3.21)
project(user_level_mmap C)

add_subdirectory(PTEditor)
add_subdirectory(userfaultfd_demo)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
