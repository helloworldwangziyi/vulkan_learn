cmake_minimum_required(VERSION 3.16)
project(vulkan_tutorial LANGUAGES CXX)

# =============================================================================
#  Vulkan 教程学习环境 —— CMake 构建
#
#  只做三件事：找到 GLFW 和 Vulkan、给 lessons/ 下每课建一个可执行文件、
#  把两套工具链的坑绕过去。没有任何封装代码。
#
#  VS Code：装 CMake Tools 扩展后打开本目录，它会读 CMakePresets.json
#  自动配好编译器和生成器，底部状态栏直接选 target 就能编译/调试。
#
#  命令行：
#      cmake --preset default
#      cmake --build --preset default
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# -----------------------------------------------------------------------------
# Vulkan SDK
# -----------------------------------------------------------------------------
set(_vulkan_candidates "")
if(DEFINED ENV{VULKAN_SDK})
    list(APPEND _vulkan_candidates "$ENV{VULKAN_SDK}")
endif()
file(GLOB _vulkan_dirs "C:/VulkanSDK/*")
list(APPEND _vulkan_candidates ${_vulkan_dirs})
list(SORT _vulkan_candidates ORDER DESCENDING)

set(VULKAN_ROOT "")
foreach(_dir ${_vulkan_candidates})
    if(EXISTS "${_dir}/Include/vulkan/vulkan.h" AND EXISTS "${_dir}/Lib/vulkan-1.lib")
        set(VULKAN_ROOT "${_dir}")
        break()
    endif()
endforeach()

if(NOT VULKAN_ROOT)
    message(FATAL_ERROR
        "找不到 Vulkan SDK（需要 Include/vulkan/vulkan.h 和 Lib/vulkan-1.lib）。\n"
        "请安装 LunarG Vulkan SDK，或设置环境变量 VULKAN_SDK 指向安装目录。")
endif()
message(STATUS "Vulkan SDK  : ${VULKAN_ROOT}")

set(VULKAN_INCLUDE_DIR "${VULKAN_ROOT}/Include")
set(VULKAN_IMPORT_LIB  "${VULKAN_ROOT}/Lib/vulkan-1.lib")

# -----------------------------------------------------------------------------
# GLFW —— 用 tools/glfw 里那份 MinGW 自己编的 libglfw3.a
#
# 不用 C:\Users\Administrator\Files\OpenGL\lib\glfw3.lib：那是 MSVC 静态库，
# MinGW 链接会以 "corrupt .drectve" + ld 返回 5 失败，因为它的 .obj 依赖
# __GSHandlerCheck / _stdio_common_vsprintf 等 MinGW CRT 里没有的 MSVC 符号。
# 重编：tools\glfw\rebuild.cmd（源码在 tools/glfw/src）。
# -----------------------------------------------------------------------------
set(GLFW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/glfw")
set(GLFW_INCLUDE_DIR "${GLFW_ROOT}/include")
set(GLFW_LIB "${GLFW_ROOT}/lib/libglfw3.a")

if(NOT EXISTS "${GLFW_LIB}")
    message(FATAL_ERROR
        "找不到 GLFW 库: ${GLFW_LIB}\n"
        "跑一下 tools\\glfw\\rebuild.cmd 从源码编译它。")
endif()
message(STATUS "GLFW        : ${GLFW_LIB}")

# -----------------------------------------------------------------------------
# 运行期依赖 vulkan-1.dll（拷到产物目录旁边，调试/直接双击都稳）
# -----------------------------------------------------------------------------
set(VULKAN_RUNTIME_DLL "")
foreach(_dll "C:/Windows/System32/vulkan-1.dll" "$ENV{VULKAN_SDK}/Bin/vulkan-1.dll")
    if(EXISTS "${_dll}")
        set(VULKAN_RUNTIME_DLL "${_dll}")
        break()
    endif()
endforeach()

# -----------------------------------------------------------------------------
# 每课一个可执行文件
#
# 新增一课：把 .cpp 放进 lessons/ 再重新 configure（GLOB 的代价）。
# VS Code 里会自动重配；命令行下再跑一次 cmake 即可。
# -----------------------------------------------------------------------------
file(GLOB LESSON_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/lessons/*.cpp")
list(SORT LESSON_SOURCES)

if(NOT LESSON_SOURCES)
    message(FATAL_ERROR "lessons/ 下没有 .cpp 文件")
endif()

foreach(lesson_src ${LESSON_SOURCES})
    get_filename_component(lesson_name "${lesson_src}" NAME_WE)

    add_executable(${lesson_name} "${lesson_src}")

    target_include_directories(${lesson_name} PRIVATE
        "${VULKAN_INCLUDE_DIR}"
        "${GLFW_INCLUDE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"   # stb_image.h / tiny_obj_loader.h
    )

    # 关键：vulkan-1.lib 必须按**全路径**交给链接器。
    # MinGW 的 ld 能读 MSVC 格式的导入库，但不认 -L <dir> + -l vulkan-1
    # 这种找法（实测 ld 返回 5）。显式带扩展名可让 CMake 原样传递，
    # 不做成 -lvulkan-1。
    target_link_libraries(${lesson_name} PRIVATE
        "${GLFW_LIB}"
        "${VULKAN_IMPORT_LIB}"
        gdi32
        user32
        shell32
        ole32
    )

    if(MSVC)
        target_compile_options(${lesson_name} PRIVATE /W3 /utf-8)
    else()
        # -static-libgcc/-static-libstdc++：Qt MinGW 默认动态链 libstdc++/libgcc，
        # 产物要直接双击运行就不能依赖 Qt 的 bin 目录在 PATH 里
        target_compile_options(${lesson_name} PRIVATE -Wall -static-libgcc -static-libstdc++)
    endif()

    set_target_properties(${lesson_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

    if(VULKAN_RUNTIME_DLL)
        add_custom_command(TARGET ${lesson_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${VULKAN_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:${lesson_name}>/vulkan-1.dll"
            COMMENT "复制 vulkan-1.dll 到产物目录"
            VERBATIM
        )
    endif()
endforeach()

list(LENGTH LESSON_SOURCES _lesson_count)
message(STATUS "课程        : ${_lesson_count} 个 -> ${LESSON_SOURCES}")
