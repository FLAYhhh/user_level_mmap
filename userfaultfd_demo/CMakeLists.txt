project(userfaultfd_demo C)

set(CMAKE_C_STANDARD 11)

add_executable(userfaultfd_demo userfaultfd_demo.c)
target_link_libraries(userfaultfd_demo pthread)
target_link_libraries(userfaultfd_demo pteditor)