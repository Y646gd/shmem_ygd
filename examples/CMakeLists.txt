# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------
set(CMAKE_LINK_DEPENDS_NO_SHARED 1)

set(CATLASS_ROOT "${PROJECT_SOURCE_DIR}/3rdparty/catlass" CACHE PATH
    "Path to the Catlass source tree")

include_directories(
    ${PROJECT_SOURCE_DIR}/src/device/
    ${PROJECT_SOURCE_DIR}/src/device_simt/
)

# catlass 适配宏
if(SOC_TYPE STREQUAL "Ascend950")
    add_compile_definitions(CATLASS_ARCH=3510)
else()
    add_compile_definitions(CATLASS_ARCH=2201)
endif()

set(ACLSHMEM_ALL_KERNEL_TARGETS "" CACHE INTERNAL "All collective kernel shared library targets")

set(PYEXPAND_SUPPORTED_TORCH
    kv_shuffle
    allgather
)

if(DEFINED ENABLE_ASCENDC_DUMP AND ENABLE_ASCENDC_DUMP)
    add_compile_definitions(ENABLE_ASCENDC_DUMP)
    add_compile_definitions(ASCENDC_DUMP=1)
    add_compile_definitions(ASCENDC_DEBUG)
    link_libraries(ascend_dump)
endif()

function(aclshmem_add_fusion_example NAME)
    if(USE_EXAMPLES)
        add_executable(${NAME} ${ARGN})
        if(DEFINED ENABLE_ASCENDC_DUMP AND ENABLE_ASCENDC_DUMP)
            target_compile_definitions(${NAME} PRIVATE ENABLE_ASCENDC_DUMP ASCENDC_DUMP=1 ASCENDC_DEBUG)
        endif()

        target_compile_options(${NAME} PRIVATE ${CMAKE_CCE_COMPILE_OPTIONS} ${CCE_AICORE_ARCH} ${SANITIZER_FLAGS})
        target_include_directories(${NAME} PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CATLASS_ROOT}/include
            ${CATLASS_ROOT}/examples/common
            ${PROJECT_SOURCE_DIR}/examples/${NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/examples/utils
            ${PROJECT_SOURCE_DIR}/src/device/
            ${PROJECT_SOURCE_DIR}/src/device_simt/
            ${PROJECT_SOURCE_DIR}/src/
            ${MPI_INCLUDE_PATH}
            $ENV{ASCEND_HOME_PATH}/tools/mssanitizer/include
        )
        target_link_options(${NAME} PRIVATE --cce-fatobj-link ${CCE_AICORE_ARCH} ${SANITIZER_FLAGS})

        if(DEFINED ENABLE_ASCENDC_DUMP AND ENABLE_ASCENDC_DUMP)
            target_link_libraries(${NAME} PRIVATE ascend_dump)
        endif()
        target_link_libraries(${NAME} PRIVATE shmem)
        target_compile_options(${NAME} PRIVATE ${MPI_CXX_COMPILE_FLAGS})
    endif()

    if(PYEXPAND_EXAMPLE AND ${NAME} IN_LIST PYEXPAND_SUPPORTED_TORCH)
        add_library(${NAME}_kernel SHARED  ${ARGN})
        if(DEFINED ENABLE_ASCENDC_DUMP AND ENABLE_ASCENDC_DUMP)
            target_compile_definitions(${NAME}_kernel PRIVATE ENABLE_ASCENDC_DUMP ASCENDC_DUMP=1 ASCENDC_DEBUG)
        endif()

        target_compile_options(${NAME}_kernel PRIVATE ${CMAKE_CCE_COMPILE_OPTIONS} --cce-aicore-arch=dav-c220 ${SANITIZER_FLAGS})
        target_include_directories(${NAME}_kernel PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CATLASS_ROOT}/include
            ${CATLASS_ROOT}/examples/common
            ${PROJECT_SOURCE_DIR}/examples/${NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/examples/utils
            ${PROJECT_SOURCE_DIR}/src/device/
            ${PROJECT_SOURCE_DIR}/src/device_simt/
            ${PROJECT_SOURCE_DIR}/src/
            ${MPI_INCLUDE_PATH}
        )
        target_link_options(${NAME}_kernel PRIVATE --cce-fatobj-link --cce-aicore-arch=dav-c220 ${SANITIZER_FLAGS})

        if(DEFINED ENABLE_ASCENDC_DUMP AND ENABLE_ASCENDC_DUMP)
            target_link_libraries(${NAME}_kernel PRIVATE ascend_dump)
        endif()
        target_link_libraries(${NAME}_kernel PRIVATE shmem)
        target_compile_options(${NAME}_kernel PRIVATE ${MPI_CXX_COMPILE_FLAGS})

        list(APPEND ACLSHMEM_ALL_KERNEL_TARGETS ${NAME}_kernel)
        set(ACLSHMEM_ALL_KERNEL_TARGETS ${ACLSHMEM_ALL_KERNEL_TARGETS} CACHE INTERNAL "Update kernel targets")
        install(TARGETS ${NAME}_kernel
                LIBRARY DESTINATION torch_binding/kernels)
    endif()

endfunction()

function(aclshmem_add_collective_example NAME)
    add_library(${NAME}_kernel SHARED ${NAME}_kernel.cpp)
    target_compile_options(${NAME}_kernel PRIVATE ${CMAKE_CCE_COMPILE_OPTIONS} ${CCE_AICORE_ARCH} ${SANITIZER_FLAGS})
    target_include_directories(${NAME}_kernel PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CATLASS_ROOT}/include
        ${CATLASS_ROOT}/examples/common
        ${PROJECT_SOURCE_DIR}/examples/${NAME}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/examples/utils
        ${PROJECT_SOURCE_DIR}/src/device/
        ${PROJECT_SOURCE_DIR}/src/device_simt/
        ${PROJECT_SOURCE_DIR}/src/
        $ENV{ASCEND_HOME_PATH}/tools/mssanitizer/include
    )
    target_link_options(${NAME}_kernel PRIVATE --cce-fatobj-link ${CCE_AICORE_ARCH} ${SANITIZER_FLAGS})
    find_library(UNIFIED_DLOG_LIB NAMES unified_dlog PATHS $ENV{ASCEND_HOME_PATH}/lib64)
    if(UNIFIED_DLOG_LIB)
        target_link_libraries(${NAME}_kernel PRIVATE unified_dlog)
    endif()
    if(USE_EXAMPLES)
        add_executable(${NAME} main.cpp)
        target_compile_options(${NAME} PRIVATE ${CMAKE_CPP_COMPILE_OPTIONS})
        target_include_directories(${NAME} PRIVATE
            ${PROJECT_SOURCE_DIR}/include
            ${CATLASS_ROOT}/examples/common
            ${PROJECT_SOURCE_DIR}/examples/${NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${PROJECT_SOURCE_DIR}/examples/utils
            ${PROJECT_SOURCE_DIR}/src/device/
            ${PROJECT_SOURCE_DIR}/src/device_simt/
            ${PROJECT_SOURCE_DIR}/src/host
            ${PROJECT_SOURCE_DIR}/src/host/bootstrap/config_store
            ${PROJECT_SOURCE_DIR}/src/host/bootstrap/config_store/acc_links/include
            ${PROJECT_SOURCE_DIR}/src/host/mem/heap
            ${MPI_INCLUDE_PATH}
        )
        target_link_libraries(${NAME} PRIVATE shmem ${NAME}_kernel)
        target_compile_options(${NAME} PRIVATE ${MPI_CXX_COMPILE_FLAGS})
    endif()

    if(PYEXPAND_EXAMPLE AND ${NAME} IN_LIST PYEXPAND_SUPPORTED_TORCH)
        list(APPEND ACLSHMEM_ALL_KERNEL_TARGETS ${NAME}_kernel)
        set(ACLSHMEM_ALL_KERNEL_TARGETS ${ACLSHMEM_ALL_KERNEL_TARGETS} CACHE INTERNAL "Update kernel targets")
        install(TARGETS ${NAME}_kernel
                LIBRARY DESTINATION torch_binding/kernels)
    endif()

endfunction()

foreach(EXAMPLE
    combine
    dispatch
    kv_shuffle
    aclgraph_demo
    allgather
    barrier_all
    multi_instance
    rma_d2h_demo
    sdma_d2h_demo
    sdma
    notifywait
    cmo
    shmem_perftest
    udma_demo
    udma_qp_demo
    udma_atomic_add
    tp_allreduce_udma
)
    add_subdirectory(${EXAMPLE})
endforeach()

add_subdirectory(topology_query)
add_subdirectory(topology_query_mte_sdma)
add_subdirectory(topology_query_mte_sdma_roce)

if(HAS_ACLRT_MEM_FABRIC_HANDLE)
    add_subdirectory(user_buffer)
endif()

if(SOC_TYPE STREQUAL "Ascend950"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shmem_mega_moe/kernel/arch35/shmem_mega_moe_tiling.h"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shmem_mega_moe/kernel/dispatch/quantize_functions.h"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/shmem_mega_moe/kernel/arch35/shmem_mega_moe_kernel.h")
    add_subdirectory(shmem_mega_moe)
endif()

if(SOC_TYPE STREQUAL "Ascend950")
    add_subdirectory(transpose_alltoall)
endif()

if(ACLSHMEM_HCCS_SIO_LINK)
    add_subdirectory(hccs_sio_link)
endif()

if(SOC_TYPE STREQUAL "Ascend950" AND ACLSHMEM_ONE_MULTI_PATH)
    add_subdirectory(one_multi_path)
endif()


if(ACLSHMEM_SIMT_SUPPORT)
foreach(
    EXAMPLE
    simt_rma
    simt_rma_scalar
    simt_rma_ub2gm
)
    add_subdirectory(${EXAMPLE})
endforeach()
endif()

if(ACLSHMEM_RDMA_SUPPORT)
    foreach(EXAMPLE
        rdma_perftest_demo
        rdma_demo
        rdma_sync_barrier_demo
        rdma_aclgraph_demo
        rdma_handlewait_test/unuse_handlewait
        rdma_handlewait_test/use_handlewait
    )
        add_subdirectory(${EXAMPLE})
endforeach()

    if(DEFINED ACLSHMEM_RDMA_BACKEND)
        string(TOUPPER "${ACLSHMEM_RDMA_BACKEND}" RDMA_BACKEND_UPPER)
        add_subdirectory(rdma_qp_demo)
        add_subdirectory(rdma_atomic_demo)
    endif()
    if(NOT DEFINED ACLSHMEM_RDMA_BACKEND OR NOT RDMA_BACKEND_UPPER STREQUAL "HNS_1825")
        add_subdirectory(rdma_aggregate_demo)
    endif()
endif()

if(PYEXPAND_EXAMPLE)
    add_subdirectory(torch_binding)
endif()
