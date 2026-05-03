cmake_minimum_required(VERSION 3.16)
project(rdma_ai_infra C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / VS Code IntelliSense

# Build all static libs with -fPIC so they can be linked into the
# pybind11 shared module (Phase 5). Negligible perf cost on x86_64.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Transport interface keeps id/offset params even when a backend ignores them
# (TCP send/recv, poll), so -Wno-unused-parameter is intentional.
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_subdirectory(phase1_verbs)
add_subdirectory(phase2_transport)
add_subdirectory(phase3_collective)
add_subdirectory(phase4_kv_cache)

# Phase 5 builds a Python extension via pybind11.
# Opt-in: set -DBUILD_PYBIND=ON since pybind11 isn't a hard dep of phases 1-4.
option(BUILD_PYBIND "Build Phase 5 pybind11 binding" ON)
if(BUILD_PYBIND)
    add_subdirectory(phase5_vllm)
endif()
