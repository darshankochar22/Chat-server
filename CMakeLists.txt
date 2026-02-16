cmake_minimum_required(VERSION 3.15)
project(trading_engine)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Boost with system component (required for WebSocket)
find_package(Boost 1.70 REQUIRED COMPONENTS system)

# Include directory (optional, if it exists)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include)
    include_directories(include)
endif()

# Main executable
add_executable(engine
    src/main.cpp
)

# Link Boost system library
target_link_libraries(engine 
    PRIVATE 
        Boost::system
)

# Add pthread for Linux (macOS doesn't need it)
if(NOT APPLE)
    target_link_libraries(engine PRIVATE pthread)
endif()