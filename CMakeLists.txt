cmake_minimum_required(VERSION 3.16)
project(vulkan_tutorial LANGUAGES CXX)

# =============================================================================
#  Vulkan 教程学习环境 —— CMake 构建（Windows / Linux / macOS）
#
#  只做三件事：找到 GLFW 和 Vulkan、给 lessons/ 下每课建一个可执行文件、
#  把各平台的坑绕过去。没有任何封装代码。
#
#  VS Code：装 CMake Tools 扩展后打开本目录，它会读 CMakePresets.json
#  自动配好编译器和生成器，底部状态栏直接选 target 就能编译/调试。
#
#  命令行：
#      cmake --preset default        # Windows（Linux/macOS 用各自 preset 或见 README）
#      cmake --build --preset default
#
#  跨平台说明：
#    * Vulkan 三个平台三种布局，优先 find_package(Vulkan)，失败退回手动探测
#    * GLFW 用 tools/glfw/src 里的官方 3.4 源码现编（Windows 仍用预编译的
#      libglfw3.a，见 README 里 MinGW 链 MSVC 库的失败史）
#    * 着色器编译（glslc → .spv）纳入构建，三个平台同一套；
#      没装 SDK 时自动跳过（已提交的 .spv 照样能用）
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# -----------------------------------------------------------------------------
# Vulkan SDK / loader
#
#   Windows : SDK 自带 Lib/vulkan-1.lib 导入库 + Include/
#   Linux   : 系统包管理器的 libvulkan.so + /usr/include，或 SDK 的 lib/ + include/
#   macOS   : LunarG SDK 的 macOS/lib/libvulkan.1.dylib + macOS/include（底层是 MoltenVK）
#
# find_package(Vulkan)（CMake 自带模块）三个平台都认识，优先用它；
# 失败再退回手动探测——Windows 扫 C:\VulkanSDK，Linux 兜底 find_library，
# macOS 额外认 Homebrew MoltenVK 的 /usr/local、/opt/homebrew。
# -----------------------------------------------------------------------------
find_package(Vulkan QUIET)

set(VULKAN_TARGET "")
set(VULKAN_RUNTIME_DLL "")
set(GLSLC_EXECUTABLE "")

if(Vulkan_FOUND)
    set(VULKAN_TARGET Vulkan::Vulkan)
    message(STATUS "Vulkan SDK  : ${Vulkan_LIBRARY} (find_package)")
    if(Vulkan_GLSLC_EXECUTABLE)
        set(GLSLC_EXECUTABLE "${Vulkan_GLSLC_EXECUTABLE}")
    endif()
    # find_package 成功也能拿到 SDK 根（Windows 下仍拷运行库，双击运行才稳）
    if(WIN32 AND DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/Bin/vulkan-1.dll")
        set(VULKAN_RUNTIME_DLL "$ENV{VULKAN_SDK}/Bin/vulkan-1.dll")
    endif()
    if(NOT VULKAN_RUNTIME_DLL AND WIN32 AND EXISTS "C:/Windows/System32/vulkan-1.dll")
        set(VULKAN_RUNTIME_DLL "C:/Windows/System32/vulkan-1.dll")
    endif()
else()
    set(_vulkan_candidates "")
    if(DEFINED ENV{VULKAN_SDK})
        list(APPEND _vulkan_candidates "$ENV{VULKAN_SDK}")
    endif()
    if(WIN32)
        file(GLOB _vulkan_dirs "C:/VulkanSDK/*")
        list(APPEND _vulkan_candidates ${_vulkan_dirs})
        list(SORT _vulkan_candidates ORDER DESCENDING)
    elseif(APPLE)
        # Homebrew MoltenVK 布局（SDK 根则自带 macOS/ 子目录，下面循环会试两种）
        list(APPEND _vulkan_candidates "/usr/local" "/opt/homebrew")
    else()
        list(APPEND _vulkan_candidates "/usr")
    endif()

    set(VULKAN_ROOT "")
    foreach(_dir ${_vulkan_candidates})
        if(WIN32)
            set(_inc_subs "Include")
            set(_lib_subs "Lib")
            set(_lib_names "vulkan-1.lib")
        elseif(APPLE)
            set(_inc_subs "macOS/include" "include")
            set(_lib_subs "macOS/lib" "lib")
            set(_lib_names "libvulkan.1.dylib" "libvulkan.dylib")
        else()
            set(_inc_subs "include" "")
            set(_lib_subs "lib" "lib/x86_64-linux-gnu")
            set(_lib_names "libvulkan.so" "libvulkan.so.1")
        endif()

        foreach(_inc_sub ${_inc_subs})
            if(EXISTS "${_dir}/${_inc_sub}/vulkan/vulkan.h")
                foreach(_lib_sub ${_lib_subs})
                    foreach(_lib_name ${_lib_names})
                        if(EXISTS "${_dir}/${_lib_sub}/${_lib_name}")
                            set(VULKAN_ROOT "${_dir}")
                            set(VULKAN_INCLUDE_DIR "${_dir}/${_inc_sub}")
                            set(VULKAN_IMPORT_LIB  "${_dir}/${_lib_sub}/${_lib_name}")
                            break()
                        endif()
                    endforeach()
                    if(VULKAN_ROOT)
                        break()
                    endif()
                endforeach()
            endif()
            if(VULKAN_ROOT)
                break()
            endif()
        endforeach()
        if(VULKAN_ROOT)
            break()
        endif()
    endforeach()

    # Linux 兜底：候选目录都没中就走系统搜索
    if(NOT VULKAN_ROOT AND NOT WIN32 AND NOT APPLE)
        find_library(VULKAN_IMPORT_LIB NAMES vulkan libvulkan.1)
        find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h)
        if(VULKAN_IMPORT_LIB AND VULKAN_INCLUDE_DIR)
            set(VULKAN_ROOT "(system)")
        endif()
    endif()

    if(NOT VULKAN_ROOT)
        message(FATAL_ERROR
            "找不到 Vulkan（头文件 vulkan/vulkan.h + 加载库）。\n"
            "  Windows : 安装 LunarG Vulkan SDK，或设置 VULKAN_SDK\n"
            "  Linux   : 装 vulkan-headers + libvulkan（如 apt install libvulkan-dev），\n"
            "            或装 LunarG SDK 并设置 VULKAN_SDK\n"
            "  macOS   : 装 LunarG Vulkan SDK（含 MoltenVK），export VULKAN_SDK=.../macOS")
    endif()
    message(STATUS "Vulkan SDK  : ${VULKAN_IMPORT_LIB}")
    set(VULKAN_TARGET "${VULKAN_IMPORT_LIB}")

    # glslc：Windows 在 SDK 的 Bin/，Linux/macOS 在 bin/
    foreach(_glslc "${VULKAN_ROOT}/Bin/glslc.exe" "${VULKAN_ROOT}/bin/glslc"
                   "$ENV{VULKAN_SDK}/Bin/glslc.exe" "$ENV{VULKAN_SDK}/bin/glslc")
        if(EXISTS "${_glslc}")
            set(GLSLC_EXECUTABLE "${_glslc}")
            break()
        endif()
    endforeach()

    # 运行期依赖 vulkan-1.dll（仅 Windows，拷到产物目录旁边，双击运行才稳）
    if(WIN32)
        foreach(_dll "C:/Windows/System32/vulkan-1.dll" "${VULKAN_ROOT}/Bin/vulkan-1.dll")
            if(EXISTS "${_dll}")
                set(VULKAN_RUNTIME_DLL "${_dll}")
                break()
            endif()
        endforeach()
    endif()
endif()

# find_package 路径下的 glslc 兜底
if(NOT GLSLC_EXECUTABLE)
    find_program(GLSLC_EXECUTABLE NAMES glslc glslc.exe)
endif()

# -----------------------------------------------------------------------------
# GLFW
#
# Windows：用 tools/glfw 里那份 MinGW 自己编的 libglfw3.a。
#   不用 C:\Users\Administrator\Files\OpenGL\lib\glfw3.lib：那是 MSVC 静态库，
#   MinGW 链接会以 "corrupt .drectve" + ld 返回 5 失败，因为它的 .obj 依赖
#   __GSHandlerCheck / _stdio_common_vsprintf 等 MinGW CRT 里没有的 MSVC 符号。
#   重编：tools\glfw\rebuild.cmd（源码在 tools/glfw/src）。
#
# Linux/macOS：没有预编译库可用，直接用 tools/glfw/src 的 GLFW 3.4 官方源码
#   现编一个静态库。源文件子集 + _GLFW_X11/_GLFW_COCOA 宏照抄上游
#   src/CMakeLists.txt 的清单（X11 扩展库 3.3 起全部 dlopen 运行期加载，
#   链接期只需要 libX11 本身 + dl + pthread）。
# -----------------------------------------------------------------------------
set(GLFW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/glfw")

if(WIN32)
    set(GLFW_TARGET "${GLFW_ROOT}/lib/libglfw3.a")
    if(NOT EXISTS "${GLFW_TARGET}")
        message(FATAL_ERROR
            "找不到 GLFW 库: ${GLFW_TARGET}\n"
            "跑一下 tools\\glfw\\rebuild.cmd 从源码编译它。")
    endif()
    message(STATUS "GLFW        : ${GLFW_TARGET}")
else()
    set(_glfw_src "${GLFW_ROOT}/src")
    set(_glfw_common
        context.c init.c input.c monitor.c platform.c vulkan.c window.c
        egl_context.c osmesa_context.c
        null_init.c null_joystick.c null_monitor.c null_window.c)

    if(APPLE)
        set(_glfw_platform
            cocoa_init.c cocoa_joystick.c cocoa_monitor.c cocoa_time.c cocoa_window.c
            posix_thread.c posix_time.c)
        set(_glfw_defines _GLFW_COCOA)
        set(_glfw_libs "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation")
    else()
        find_package(X11 REQUIRED)
        set(_glfw_platform
            x11_init.c x11_monitor.c x11_window.c xkb_unicode.c glx_context.c
            linux_joystick.c posix_module.c posix_poll.c posix_thread.c posix_time.c)
        set(_glfw_defines _GLFW_X11)
        set(_glfw_libs ${X11_LIBRARIES} dl pthread m)
    endif()

    set(_glfw_sources "")
    foreach(_f ${_glfw_common} ${_glfw_platform})
        list(APPEND _glfw_sources "${_glfw_src}/${_f}")
    endforeach()

    add_library(glfw3 STATIC ${_glfw_sources})
    target_include_directories(glfw3 PUBLIC "${GLFW_ROOT}/include" PRIVATE "${_glfw_src}")
    target_compile_definitions(glfw3 PRIVATE ${_glfw_defines})
    target_link_libraries(glfw3 PUBLIC ${_glfw_libs})
    set(GLFW_TARGET glfw3)
    message(STATUS "GLFW        : tools/glfw/src 现编 (GLFW 3.4, ${_glfw_defines})")
endif()

# -----------------------------------------------------------------------------
# 着色器：glslc 离线编译 .vert/.frag → .spv，输出回 shaders/ 源目录
# （教程约定 shader.vert → vert.spv）。找不到 glslc 就跳过——
# 已提交的 .spv 仍能正常构建，改了着色器再手动跑 shaders/build.cmd（Windows）
# 或装 Vulkan SDK 后重跑 cmake。
# -----------------------------------------------------------------------------
if(GLSLC_EXECUTABLE)
    message(STATUS "glslc       : ${GLSLC_EXECUTABLE}")
    file(GLOB_RECURSE SHADER_SOURCES RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
         "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
         "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag")
    set(SPV_OUTPUTS "")
    foreach(_shader ${SHADER_SOURCES})
        get_filename_component(_dir  "${_shader}" DIRECTORY)
        get_filename_component(_name "${_shader}" NAME)
        get_filename_component(_ext  "${_shader}" EXT)
        string(REGEX REPLACE "^\\." "" _ext "${_ext}")     # .vert → vert
        if(_name MATCHES "^shader\\.(vert|frag)$")
            set(_spv_name "${_ext}.spv")                   # 教程约定：shader.vert → vert.spv
        else()
            string(REGEX REPLACE "\\.(vert|frag)$" "" _base "${_name}")
            set(_spv_name "${_base}.spv")
        endif()
        if(_dir)
            set(_spv "${_dir}/${_spv_name}")
        else()
            set(_spv "${_spv_name}")
        endif()

        add_custom_command(
            OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/${_spv}"
            COMMAND "${GLSLC_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}"
                    -o "${CMAKE_CURRENT_SOURCE_DIR}/${_spv}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_shader}"
            COMMENT "glslc ${_shader} → ${_spv}"
            VERBATIM)
        list(APPEND SPV_OUTPUTS "${CMAKE_CURRENT_SOURCE_DIR}/${_spv}")
    endforeach()

    if(SPV_OUTPUTS)
        add_custom_target(shaders ALL DEPENDS ${SPV_OUTPUTS})
    endif()
else()
    message(STATUS "glslc       : 未找到，跳过着色器自动编译（已提交的 .spv 仍可用）")
endif()

# 运行期资源（shaders/*.spv、textures/）拷到产物目录旁边：
# 课程按相对路径读文件，这样从 build/ 里双击 exe 也能跑
foreach(_asset_dir shaders textures)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_asset_dir}")
        add_custom_target(copy_${_asset_dir} ALL
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/${_asset_dir}"
                    "${CMAKE_BINARY_DIR}/${_asset_dir}"
            COMMENT "复制 ${_asset_dir}/ 到产物目录"
            VERBATIM)
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

if(WIN32)
    set(PLATFORM_LIBS gdi32 user32 shell32 ole32)
else()
    set(PLATFORM_LIBS "")
endif()

foreach(lesson_src ${LESSON_SOURCES})
    get_filename_component(lesson_name "${lesson_src}" NAME_WE)

    add_executable(${lesson_name} "${lesson_src}")

    target_include_directories(${lesson_name} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"   # stb_image.h / tiny_obj_loader.h
        "${GLFW_ROOT}/include"                        # GLFW（Windows 下是裸静态库路径，不会自动带上）
    )
    if(NOT Vulkan_FOUND)
        target_include_directories(${lesson_name} PRIVATE "${VULKAN_INCLUDE_DIR}")
    endif()

    # 关键（Windows）：vulkan-1.lib 必须按**全路径**交给链接器。
    # MinGW 的 ld 能读 MSVC 格式的导入库，但不认 -L <dir> + -l vulkan-1
    # 这种找法（实测 ld 返回 5）。显式带扩展名可让 CMake 原样传递，
    # 不做成 -lvulkan-1。
    target_link_libraries(${lesson_name} PRIVATE
        ${GLFW_TARGET}
        ${VULKAN_TARGET}
        ${PLATFORM_LIBS}
    )

    if(MSVC)
        target_compile_options(${lesson_name} PRIVATE /W3 /utf-8)
    elseif(WIN32)
        # -static-libgcc/-static-libstdc++：Qt MinGW 默认动态链 libstdc++/libgcc，
        # 产物要直接双击运行就不能依赖 Qt 的 bin 目录在 PATH 里
        target_compile_options(${lesson_name} PRIVATE -Wall -static-libgcc -static-libstdc++)
    else()
        target_compile_options(${lesson_name} PRIVATE -Wall)
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
