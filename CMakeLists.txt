cmake_minimum_required(VERSION 3.14)
project(c_plus_practise)

set(CMAKE_CXX_STANDARD 11)

add_executable(c_plus_practise main.cpp transport/TcpServer.cpp transport/TcpServer.h)