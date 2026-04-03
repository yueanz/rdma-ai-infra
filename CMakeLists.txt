cmake_minimum_required(VERSION 3.16)
project(rdma_ai_infra C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)

add_subdirectory(phase1_verbs)
add_subdirectory(phase2_transport)
