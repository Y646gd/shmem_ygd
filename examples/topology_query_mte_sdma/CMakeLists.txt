# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
if(USE_EXAMPLES)
    add_executable(topology_query_mte_sdma main.cpp)
    target_compile_options(topology_query_mte_sdma PRIVATE ${CMAKE_CPP_COMPILE_OPTIONS})
    target_include_directories(topology_query_mte_sdma PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/examples/topology_query)
    target_link_libraries(topology_query_mte_sdma PRIVATE shmem)
endif()
