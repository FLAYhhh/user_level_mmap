# 获取所有.c文件
file(GLOB SOURCES "*.c")

# 为每个.c文件生成一个可执行文件
foreach(source ${SOURCES})
    # 获取文件名（不含扩展名）
    get_filename_component(name ${source} NAME_WE)
    # 添加可执行文件
    add_executable(${name} ${source})
endforeach()