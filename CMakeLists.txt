cmake_minimum_required(VERSION 3.20)
project(riskcore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# nlohmann/json via FetchContent
include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(json)

# OpenSSL for WebSocket SHA1 handshake
find_package(OpenSSL REQUIRED)

add_executable(riskcore
  src/main.cpp
  src/risk_engine.cpp
  src/data_loader.cpp
  src/ws_server.cpp
)

target_include_directories(riskcore PRIVATE src)
target_link_libraries(riskcore PRIVATE nlohmann_json::nlohmann_json OpenSSL::Crypto OpenSSL::SSL)
target_compile_options(riskcore PRIVATE -O2 -Wall -Wextra)
