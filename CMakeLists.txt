cmake_minimum_required(VERSION 3.10)
project(WebRTC_Test_Project)

# Specify Clang as the compiler
set(CMAKE_C_COMPILER "/usr/bin/clang")
set(CMAKE_CXX_COMPILER "/usr/bin/clang++")

# Set C++ standard to C++17
set(CMAKE_CXX_STANDARD 17)

# Add necessary definitions
add_definitions(-DWEBRTC_MAC=1)
add_definitions(-DWEBRTC_POSIX=1)
add_definitions(-D__STDC_CONSTANT_MACROS=1)

# Find OpenCV
find_package(OpenCV REQUIRED)

message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
# Include OpenCV include directories
include_directories(${OpenCV_INCLUDE_DIRS})
link_directories("/opt/homebrew/Cellar/opencv/4.10.0_12/lib")

# Path to WebRTC build
set(WEBSRTC_DIR "/Users/hungnghiem/Desktop/C++/Document/c++/webrtc/webrtc_library")

# Include directories for WebRTC headers
include_directories(${WEBSRTC_DIR}/include)
include_directories(${WEBSRTC_DIR}/include/third_party/abseil-cpp)
link_directories(${WEBSRTC_DIR}/lib)

# Include OpenSSL
find_package(OpenSSL REQUIRED)
include_directories(${OPENSSL_INCLUDE_DIR})
link_directories(${OPENSSL_LIBRARIES})

# Include Boost
include_directories("/opt/homebrew/Cellar/boost/1.84.0/include")
link_directories("/opt/homebrew/Cellar/boost/1.84.0/lib")

# Set compiler flags for debugging
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -W -Wall -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g")  # Debugging flags
set(CMAKE_BUILD_TYPE Debug)  # Ensure Debug build type

# Create target for the project
add_executable(webrtc_test_project main.cpp)

# Link WebRTC libraries, Boost, and necessary frameworks
target_link_libraries(webrtc_test_project
    webrtc
    pthread
    boost_system
    boost_filesystem
    ${OpenCV_LIBS}
    "-framework Foundation"
    "-framework CoreAudio"
    "-framework CoreMedia"
    "-framework CoreGraphics"
    "-framework CoreFoundation"
    "-framework AVFoundation"
    "-framework AudioToolbox"
    "-framework VideoToolbox"
    "-framework Network"
)
