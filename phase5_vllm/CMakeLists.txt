# pybind11 binding for the Transport layer.
#
# Output: a Python extension module `rai_rdma._core` (.so) that wraps
# Phase 2's Transport interface so it can be called from Python (and
# eventually from vLLM's KVConnectorBase_V1).

# Find pybind11 from the active Python environment (venv).
# `pybind11.get_cmake_dir()` returns the path; pass it via -Dpybind11_DIR
# or rely on Python -m pybind11 --cmakedir at configure time.
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -m pybind11 --cmakedir
    OUTPUT_VARIABLE pybind11_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
find_package(pybind11 REQUIRED)

# The extension module
pybind11_add_module(rai_rdma_core MODULE
    bindings/transport_py.cpp
)

# Link against Phase 2 transport (which transitively pulls Phase 1 verbs).
target_link_libraries(rai_rdma_core PRIVATE transport)

# pybind11_add_module names the target rai_rdma_core but we want the
# resulting .so to be importable as `rai_rdma._core`. Set output name + dir.
set_target_properties(rai_rdma_core PROPERTIES
    OUTPUT_NAME "_core"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/rai_rdma"
)
