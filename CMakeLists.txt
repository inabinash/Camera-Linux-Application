cmake_minimum_required(VERSION 3.16)

project(camera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui)

add_executable(camera main.cpp)
target_link_libraries(camera PRIVATE ${OpenCV_LIBS})
