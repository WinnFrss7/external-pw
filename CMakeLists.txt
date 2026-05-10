cmake_minimum_required(VERSION 3.20)
project(IMIHAX LANGUAGES CXX RC)  # Add RC here

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(OpenGL REQUIRED)

add_executable(IMIHAX main.cpp resource.rc)  # Add resource.rc here

target_link_libraries(IMIHAX PRIVATE
    imgui::imgui
    glfw
    glad::glad
    OpenGL::GL
)

target_include_directories(IMIHAX PRIVATE ${CMAKE_SOURCE_DIR})