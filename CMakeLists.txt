cmake_minimum_required(VERSION 3.15)
project(RadarApp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ------------------ Include directories ------------------
include_directories(
    include
)

# ------------------ Matplot++ ------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS     OFF CACHE BOOL "" FORCE)
add_subdirectory(external/matplotplusplus)

# ------------------ GLAD ------------------
add_library(glad STATIC external/glad/src/glad.c)
target_include_directories(glad PUBLIC external/glad/include)

# ------------------ GLFW ------------------
file(GLOB GLFW_SOURCES external/glfw/src/*.c)
add_library(glfw STATIC ${GLFW_SOURCES})
target_include_directories(glfw PUBLIC external/glfw/include)

# ------------------ ImGui ------------------
file(GLOB IMGUI_SOURCES
    external/imgui/*.cpp
    external/imgui/backends/imgui_impl_glfw.cpp
    external/imgui/backends/imgui_impl_opengl3.cpp
)
add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    external/imgui
    external/imgui/backends
    external/glfw/include
    external/glad/include
)

# ------------------ OpenGL ------------------
find_package(OpenGL REQUIRED)

# ------------------ NetApp Server ------------------
add_executable(NetAppServer WIN32
    src/Server/NetAppServer.cpp
    src/Server/Thread/workerThread.cpp
    src/Server/Thread/acceptThread.cpp
    src/Server/Thread/networkManagerServer.cpp
)

target_link_libraries(NetAppServer PRIVATE
    glad
    imgui
    "${CMAKE_SOURCE_DIR}/external/glfw/build_glfw/src/libglfw3.a"
    OpenGL::GL
    opengl32
    winmm
    ws2_32
    ole32
    setupapi
    matplot
)

if(WIN32)
    target_link_libraries(NetAppServer PRIVATE opengl32)
endif()

# ------------------ NetApp Client ------------------
add_executable(NetAppClient WIN32
    src/Client/NetAppClient.cpp
    src/Client/Thread/networkManagerClient.cpp
)

target_link_libraries(NetAppClient PRIVATE
    glad
    imgui
    "${CMAKE_SOURCE_DIR}/external/glfw/build_glfw/src/libglfw3.a"
    OpenGL::GL
    opengl32
    winmm
    ws2_32
    ole32
    setupapi
    matplot
)

if(WIN32)
    target_link_libraries(NetAppClient PRIVATE opengl32)
endif()
