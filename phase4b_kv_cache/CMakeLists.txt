set(COMMON_INC ${CMAKE_SOURCE_DIR}/common/include)
set(TRANSPORT_INC ${CMAKE_SOURCE_DIR}/phase2_transport/include)

add_executable(kv_server src/kv_server.cpp)
target_include_directories(kv_server PRIVATE include ${COMMON_INC} ${TRANSPORT_INC})
target_link_libraries(kv_server transport)

add_executable(kv_bench bench/kv_bench.cpp)
target_include_directories(kv_bench PRIVATE include ${COMMON_INC} ${TRANSPORT_INC})
target_link_libraries(kv_bench transport)
