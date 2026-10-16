/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#ifndef SHMEMI_INIT_BACKEND_H
#define SHMEMI_INIT_BACKEND_H

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "host/init/shmem_host_init.h"
#include "host_device/shmem_common_types.h"

#include "mem/shmemi_mgr.h"

#include "init/bootstrap/shmemi_bootstrap.h"
#include "init/shmemi_user_buffer_heap.h"

#include "hybm_def.h"

#include "utils/mstx/mstx_mem_register.h"

namespace shm {
namespace transport {
struct UdmaQpConfig;
struct TopologyContext;
} // namespace transport
} // namespace shm

typedef struct entity_member {
    entity_member();
    ~entity_member();
#ifdef ACLSHMEM_SOC_950
    std::unique_ptr<shm::transport::TopologyContext> topology_context;
    std::vector<std::vector<aclshmemx_net_layer_t>> topology_net_layers_by_pe;
#endif
    void* hbm_gva = nullptr;
    void* dram_gva = nullptr;

    hybm_entity_t hbm_entity = nullptr;
    hybm_entity_t dram_entity = nullptr;
    hybm_mem_slice_t hbm_slice = nullptr;
    hybm_mem_slice_t dram_slice = nullptr;

    std::unique_ptr<shm::UserBufferHeapInput> user_buffer_heap_input;
    std::vector<int32_t> trusted_pids;

    aclshmemx_init_attr_t* entity_attr;
    aclshmemi_bootstrap_handle_t* entity_boot_handle;
    aclshmem_device_host_state_t* entity_host_state;
    aclshmem_device_host_state_t* entity_device_state;
    uint32_t udma_qp_num{1};
    std::map<uint32_t, uint32_t> udma_net_layer_policy{};
    bool udma_multi_link_enabled{false};
    uint32_t rdma_qp_num{1};
    uint32_t sdma_qp_num{1};
} entity_member;

class aclshmemi_init_backend {
public:
    aclshmemi_init_backend();
    ~aclshmemi_init_backend();

    int init_device_state();
    int finalize_device_state();
    int update_device_state(void* host_ptr, size_t size);

    int reserve_heap(aclshmem_mem_type_t mem_type = DEVICE_SIDE);
    int setup_heap(aclshmem_mem_type_t mem_type = DEVICE_SIDE);
    int remove_heap(aclshmem_mem_type_t mem_type = DEVICE_SIDE);
    int release_heap(aclshmem_mem_type_t mem_type = DEVICE_SIDE);

    int aclshmemi_control_barrier_all();
    int is_alloc_size_symmetric(size_t size);

    int bind_aclshmem_entity(
        aclshmemx_init_attr_t* attr, aclshmem_device_host_state_t* state, aclshmemi_bootstrap_handle_t* handle,
        std::unique_ptr<shm::UserBufferHeapInput> user_buffer_heap_input,
        const shm::transport::UdmaQpConfig& udma_qp_config, uint32_t rdma_qp_num, uint32_t sdma_qp_num);
    void* get_buffer_ptr(const void* local_ptr);
    int release_aclshmem_entity(uint64_t instance_id);
    int build_topology_state(uint64_t instance_id);
#if defined(ACLSHMEM_UNIT_TEST) && defined(ACLSHMEM_SOC_950)
    // TEMP_TOPOLOGY_JSON_STUB: inject discovery only; build/query remain production code.
    int install_topology_context_for_test(
        uint64_t instance_id, std::unique_ptr<shm::transport::TopologyContext> context);
#endif

    int query_topology(uint64_t instance_id, int pe, uint32_t query_mask, aclshmemx_topology_t* out);
    int get_qp_net_layer(
        uint64_t instance_id, data_op_engine_type_t engine, uint32_t peer, uint32_t qp_idx, uint32_t& net_layer);

private:
    int reach_info_init(void*& gva);
    int create_entity(aclshmem_mem_type_t mem_type = DEVICE_SIDE);
    int exchange_slice(aclshmem_mem_type_t mem_type = DEVICE_SIDE);
    int exchange_user_buffer_heap();
    int exchange_entity(aclshmem_mem_type_t mem_type = DEVICE_SIDE);

private:
    int device_id;
    int hybm_init_count = 0;

    shm::mstx_mem_register_base* mstx_reg_ptr_ = nullptr;

    std::mutex entity_map_mutex_;
    std::map<uint64_t, entity_member*> entity_map_ = {};
};

#endif // SHMEMI_INIT_BACKEND_H
