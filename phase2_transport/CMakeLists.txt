find_package(IBVerbs QUIET)
if(NOT IBVerbs_FOUND)
    find_library(IBVERBS_LIB ibverbs)
    find_path(IBVERBS_INCLUDE infiniband/verbs.h)
endif()

set(COMMON_INC ${CMAKE_SOURCE_DIR}/common/include)

# transport static library
add_library(transport STATIC
    src/rdma_backend.cpp
    src/tcp_backend.cpp
)
target_include_directories(transport PUBLIC
    include
    ${CMAKE_SOURCE_DIR}/phase1_verbs/include
    ${COMMON_INC}
)
target_link_libraries(transport rdma_verbs)

# benchmarks
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/backend_compare.cpp)
    add_executable(backend_compare bench/backend_compare.cpp)
    target_include_directories(backend_compare PRIVATE include ${COMMON_INC}
        ${CMAKE_SOURCE_DIR}/phase1_verbs/include)
    target_link_libraries(backend_compare transport)
endif()
