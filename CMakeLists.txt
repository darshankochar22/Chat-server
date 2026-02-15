cmake_minimum_required(VERSION 3.15)
project(trading_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Boost REQUIRED)

include_directories(include)

add_executable(engine
    src/main.cpp
)

target_link_libraries(engine Boost::boost)
