cmake_minimum_required(VERSION 3.16)
project(ak820-controller LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIDAPI REQUIRED hidapi-hidraw)
find_package(X11 REQUIRED)

add_executable(ak820-controller main.cpp)

target_include_directories(ak820-controller PRIVATE ${HIDAPI_INCLUDE_DIRS} ${X11_INCLUDE_DIR})
target_link_libraries(ak820-controller PRIVATE
    Qt6::Widgets Qt6::Network
    ${HIDAPI_LIBRARIES}
    ${X11_LIBRARIES})
