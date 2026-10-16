# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
if(USE_EXAMPLES)
    add_executable(topology_query main.cpp)
    target_compile_options(topology_query PRIVATE ${CMAKE_CPP_COMPILE_OPTIONS})
    target_include_directories(topology_query PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/examples/utils ${CATLASS_ROOT}/examples/common)
    target_link_libraries(topology_query PRIVATE shmem)
    if(SOC_TYPE STREQUAL "Ascend950")
        # Reuse the tested data-copy/check/cleanup flow and device kernel.
        add_executable(topology_layers_demo ../udma_qp_demo/main.cpp)
        target_compile_definitions(topology_layers_demo PRIVATE ACLSHMEM_TOPOLOGY_DEMO=1)
        target_compile_options(topology_layers_demo PRIVATE ${CMAKE_CPP_COMPILE_OPTIONS})
        target_include_directories(topology_layers_demo PRIVATE
            ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/examples/utils ${CATLASS_ROOT}/examples/common)
        target_link_libraries(topology_layers_demo PRIVATE shmem udma_qp_demo_kernel)
    endif()
endif()
