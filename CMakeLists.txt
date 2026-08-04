cmake_minimum_required(VERSION 3.20)
project(TCPServer)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS system)
find_package(Threads REQUIRED)

add_executable(TCPServer src/server.cpp)
add_executable(LoadGenerator src/load_gen.cpp)

target_link_libraries(TCPServer PRIVATE Boost::system)
target_link_libraries(LoadGenerator PRIVATE Threads::Threads)