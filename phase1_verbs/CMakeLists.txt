find_package(IBVerbs QUIET)
if(NOT IBVerbs_FOUND)
    find_library(IBVERBS_LIB ibverbs)
    find_path(IBVERBS_INCLUDE infiniband/verbs.h)
endif()

# common headers
set(COMMON_INC ${CMAKE_SOURCE_DIR}/common/include)

# phase1 static library
add_library(rdma_verbs STATIC
    src/rdma_context.c
    src/rdma_mr.c
    src/rdma_qp.c
    src/rdma_connect.c
    src/rdma_cm_connect.c
    src/rdma_ops.c
)
target_include_directories(rdma_verbs PUBLIC include ${COMMON_INC})
target_link_libraries(rdma_verbs ${IBVERBS_LIB} rdmacm)

# benchmarks
foreach(bench lat_send_recv lat_rdma_write bw_rdma_write)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/${bench}.c)
        add_executable(${bench} bench/${bench}.c)
        target_link_libraries(${bench} rdma_verbs)
    endif()
endforeach()
