cmake_minimum_required(VERSION 3.21)

# 指定源文件
file(GLOB SOURCES "*.c" "*.cpp")
file(GLOB_RECURSE HEADER_FILES "*.h")

# 生成静态库
add_library(pteditor STATIC ${SOURCES})
# 设置库的接口头文件路径
target_include_directories(pteditor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# 为了方便其他项目使用这个库，可以将库的安装路径设置为系统目录
# 或者其他项目可以找到的目录
install(TARGETS pteditor
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

# 可选：安装头文件
install(FILES ${HEADER_FILES} DESTINATION include)

add_subdirectory(demos)