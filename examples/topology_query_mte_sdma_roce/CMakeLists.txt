# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
if(USE_EXAMPLES AND ACLSHMEM_RDMA_SUPPORT)
    add_executable(topology_query_mte_sdma_roce main.cpp)
    target_compile_options(topology_query_mte_sdma_roce PRIVATE ${CMAKE_CPP_COMPILE_OPTIONS})
    target_include_directories(topology_query_mte_sdma_roce PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/examples/topology_query)
    target_link_libraries(topology_query_mte_sdma_roce PRIVATE shmem)
endif()
