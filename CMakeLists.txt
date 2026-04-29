cmake_minimum_required(VERSION 3.16)
project(rdma_ai_infra C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # for clangd / VS Code IntelliSense
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
