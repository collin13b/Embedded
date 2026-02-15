cmake_minimum_required(VERSION 3.10)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_COMPILER "aarch64-linux-gnu-g++")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(linux_Ai)
set(CMAKE_BUILD_TYPE Debug)


#查找库文件
find_package(OpenCV REQUIRED)
#输出opencv的信息
message(STATUS "OPENCV LIB STATUS:")
message(STATUS "OPENCV VERSION ${OpenCV_VERSION}")
set(3RDPARTY ${CMAKE_SOURCE_DIR}/3rdparty)
set(RGA_INCLUDE ${3RDPARTY}/rga/RK3588/include)
set(RGA_LIB ${3RDPARTY}/rga/RK3588/lib/Linux/aarch64/librga.so)

set(RKNN_INCLUDE ${3RDPARTY}/librknn_api/include)
set(RKNN_LIB ${3RDPARTY}/librknn_api/aarch64/librknnrt.so)

#包含头文件
include_directories(
        ${OpenCV_INCLUDE_DIRS}
        ${PROJECT_SOURCE_DIR}/app/inc
        ${RGA_INCLUDE}
        ${RKNN_INCLUDE}
)
set(C_DIR ${CMAKE_SOURCE_DIR}/app/src)
add_executable(app 
        ${C_DIR}/main.cpp
        ${C_DIR}/thread_pool.cpp
        ${C_DIR}/yolov5s.cpp
        ${C_DIR}/bind_thread.cpp
        )
# 将 OpenCV 的库与目标可执行文件 app 链接，确保在程序运行时能够调用 OpenCV 函数。
target_link_libraries(app
         ${OpenCV_LIBS}
         ${RKNN_LIB}
         ${RGA_LIB}

)
