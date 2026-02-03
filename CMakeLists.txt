cmake_minimum_required(VERSION 3.10)
project(linux_Ai)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_COMPILER "g++-14")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#查找库文件
find_package(OpenCV REQUIRED)
#输出opencv的信息
message(STATUS "OPENCV LIB STATUS:")
message(STATUS "OPENCV VERSION ${OpenCV_VERSION}")
#包含头文件
include_directories(
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/app/inc
)
set(C_DIR ${CMAKE_SOURCE_DIR}/app/src)
add_executable(app 
        ${C_DIR}/main.cpp
        ${C_DIR}/thread_pool.cpp
        )
# 将 OpenCV 的库与目标可执行文件 app 链接，确保在程序运行时能够调用 OpenCV 函数。
target_link_libraries(app
         ${OpenCV_LIBS}
)
