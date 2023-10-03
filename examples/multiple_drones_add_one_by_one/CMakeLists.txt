cmake_minimum_required(VERSION 3.10.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(multiple_drones_add_one_by_one)

add_executable(multiple_drones_add_one_by_one
    multiple_drones_add_one_by_one.cpp
)

find_package(MAVSDK REQUIRED)
find_package(Threads REQUIRED)

target_link_libraries(multiple_drones_add_one_by_one
    MAVSDK::mavsdk
    Threads::Threads
)

if(NOT MSVC)
    add_compile_options(multiple_drones_add_one_by_one PRIVATE -Wall -Wextra)
else()
    add_compile_options(multiple_drones_add_one_by_one PRIVATE -WX -W2)
endif()
