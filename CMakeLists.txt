cmake_minimum_required(VERSION 3.16)
project(WeChatNotifierHook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    add_compile_options(/utf-8 /W3 /wd4996)
    add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF>)
endif()

add_executable(WeChatNotifier
    src/main.cpp
    resource.rc
)

target_include_directories(WeChatNotifier PRIVATE src)

target_link_libraries(WeChatNotifier PRIVATE
    windowsapp
    winhttp
    ws2_32
    shell32
    ole32
    runtimeobject
    user32
    advapi32
)

set_target_properties(WeChatNotifier PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/build/out"
)

# 静态链接 CRT，避免目标机缺 vcruntime
if(MSVC)
    set_target_properties(WeChatNotifier PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
