cmake_minimum_required(VERSION 3.20)
project(caf-message-behavior-demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Pull CAF via FetchContent -------------------------------------------
include(FetchContent)

FetchContent_Declare(
  CAF
  GIT_REPOSITORY https://github.com/actor-framework/actor-framework.git
  GIT_SHALLOW    TRUE
  # Pin to a known-good commit (v1.1.0 pre-release).  Update as needed.
  GIT_TAG        75d51522969969565e6a30a64727d0fcd6d232f2
  # ---- local override (comment out GIT_* above, uncomment below) ----
  # SOURCE_DIR    /root/actor-framework
)

# Options needed by CAF's CMake: disable IO / Networking so we only build
# libcaf_core (ships message-passing, behavior, serialization, etc.).
set(CAF_ENABLE_IO_EXAMPLES OFF  CACHE BOOL "" FORCE)
set(CAF_ENABLE_NET_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(CAF_ENABLE_CURL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CAF_ENABLE_PROTOBUF_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CAF_ENABLE_QT6_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CAF_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(CAF)

# ---- Demo executable ----------------------------------------------------
add_executable(message-behavior-demo main.cpp)
target_link_libraries(message-behavior-demo PRIVATE CAF::internal CAF::core)
