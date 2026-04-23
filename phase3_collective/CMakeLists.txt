set(COMMON_INC ${CMAKE_SOURCE_DIR}/common/include)
set(TRANSPORT_INC ${CMAKE_SOURCE_DIR}/phase2_transport/include)

add_library(collective STATIC
    src/world.cpp
    src/ring_allreduce.cpp
)
target_include_directories(collective PUBLIC include ${COMMON_INC} ${TRANSPORT_INC})
target_link_libraries(collective transport pthread)

foreach(bench allreduce_bench)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/${bench}.cpp)
        add_executable(${bench} bench/${bench}.cpp)
        target_include_directories(${bench} PRIVATE include ${COMMON_INC} ${TRANSPORT_INC})
        target_link_libraries(${bench} collective transport)
    endif()
endforeach()
