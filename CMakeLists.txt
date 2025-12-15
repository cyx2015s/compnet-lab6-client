cmake_minimum_required(VERSION 4.0)

project(compnet_client)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_COMPILER "clang++")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 将imgui库链接到client目标
set(IMGUI_SOURCES
    imgui/imgui.cpp
    imgui/imgui_demo.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
)

# 添加imgui的实现文件（例如OpenGL3和GLFW）
list(APPEND IMGUI_SOURCES
    imgui/backends/imgui_impl_opengl3.cpp
    imgui/backends/imgui_impl_glfw.cpp
)

# 将源文件添加到client目标
set(SOURCES
    src/main.cpp
    ${IMGUI_SOURCES}
)

# 将源文件链接到client目标
add_executable(client ${SOURCES})

include_directories(
    imgui
    imgui/backends
)

# 链接必要的库
# sudo apt install libopengl-dev
find_package(OpenGL REQUIRED)
# sudo apt install libglfw3-dev
find_package(glfw3 REQUIRED)

target_link_libraries(client PRIVATE OpenGL::GL glfw)

