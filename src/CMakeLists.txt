set(SOURCE_FILES user_level_mmap.cc
        user_level_mmap.cc)
add_library(user_level_mmap STATIC ${SOURCE_FILES}
        user_level_mmap.cc)
target_compile_options(user_level_mmap PRIVATE -Werror)

target_include_directories(user_level_mmap PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(user_level_mmap pthread)
target_link_libraries(user_level_mmap pteditor)
target_link_libraries(user_level_mmap jemalloc)

install(TARGETS user_level_mmap
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)

file(GLOB HEADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../include/*.h")
install(FILES ${HEADER_FILES} DESTINATION include)
