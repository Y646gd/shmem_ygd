/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */
#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <iostream>
#include <sstream>
#include <functional>
#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include "acl/acl.h"
#include "acl/acl_rt.h"
#include "acl/error_codes/rt_error_codes.h"
#include "shmemi_host_common.h"
#include "shmemi_init.h"
#include "shmemi_user_buffer_heap.h"
#include "transport_def.h"
#include "host/shmem_host_def.h"
#include "prof/prof_util.h"
#include "shmemi_scope_guard.h"
#include "utils/exception/shmem_exception_report.h"
#include "utils/under_api/dl_api.h"
#include "utils/under_api/dl_acl_api.h"

#define DEFAULT_MY_PE (-1)
#define DEFAULT_N_PES (-1)

constexpr int DEFAULT_TEVENT = 0;
constexpr int DEFAULT_BLOCK_NUM = 1;

constexpr uint32_t DEFAULT_MTE_UB_SIZE = 16 * 1024;
constexpr uint32_t DEFAULT_SDMA_UB_SIZE = 64;
constexpr int64_t DEFAULT_SDMA_UB_OFFSET = 191 * 1024;
constexpr uint32_t DEFAULT_RDMA_UB_SIZE = ACLSHMEM_RDMA_MTE_STAGING_UB_SIZE;
constexpr int64_t DEFAULT_RDMA_UB_OFFSET = 190 * 1024;
// UDMA PIPE_MTE3 stages one full WQE block in UB; 128 B covers current data-mover opcodes.
constexpr uint32_t DEFAULT_UDMA_UB_SIZE = ACLSHMEM_UDMA_MTE_STAGING_UB_SIZE;
constexpr int64_t DEFAULT_UDMA_UB_OFFSET = 189 * 1024;

static_assert(
    512ULL + SYNC_POOL_SIZE + SYNC_COUNTERS_SIZE <= ACLSHMEM_EXTRA_SIZE,
    "ACLSHMEM_EXTRA_SIZE must cover all allocator-backed internal control objects");

// initializer
#define ACLSHMEM_DEVICE_HOST_STATE_INITIALIZER                                                    \
    {                                                                                             \
        (1 << 16) + sizeof(aclshmem_device_host_state_t),  /* version */                          \
        (DEFAULT_MY_PE),                                   /* mype */                             \
        (DEFAULT_N_PES),                                   /* npes */                             \
        NULL,                                              /* heap_base */                        \
        NULL,                                              /* host_heap_base */                   \
        NULL,                                              /* p2p_device_heap_base */             \
        NULL,                                              /* rdma_device_heap_base */            \
        NULL,                                              /* sdma_device_heap_base */            \
        NULL,                                              /* p2p_heap_host_base */               \
        NULL,                                              /* rdma_heap_host_base */              \
        NULL,                                              /* sdma_heap_host_base */              \
        {},                                                /* topo_list */                        \
        SIZE_MAX,                                          /* heap_size */                        \
        {NULL},                                            /* team_pools */                       \
        0,                                                 /* sync_pool */                        \
        0,                                                 /* sync_counter */                     \
        0,                                                 /* core_sync_pool */                   \
        0,                                                 /* core_sync_counter */                \
        false,                                             /* aclshmem_is_aclshmem_initialized */ \
        false,                                             /* aclshmem_is_aclshmem_created */     \
        {0, DEFAULT_MTE_UB_SIZE, 0},                       /* aclshmem_mte_config */              \
        {DEFAULT_SDMA_UB_OFFSET, DEFAULT_SDMA_UB_SIZE, 0}, /* aclshmem_sdma_config */             \
        {DEFAULT_RDMA_UB_OFFSET, DEFAULT_RDMA_UB_SIZE, 0}, /* aclshmem_rdma_config */             \
        {DEFAULT_UDMA_UB_OFFSET, DEFAULT_UDMA_UB_SIZE, 0}, /* aclshmem_udma_config */             \
        0,                                                 /* qp_info */                          \
        0,                                                 /* sdma_workspace_addr */              \
        NULL,                                              /* aclshmem_prof_pe_t */               \
        0,                                                 /* signal_addr */                      \
    }

aclshmem_device_host_state_t g_state = ACLSHMEM_DEVICE_HOST_STATE_INITIALIZER;
aclshmem_host_state_t g_state_host = {nullptr, DEFAULT_TEVENT, DEFAULT_BLOCK_NUM, 0};

// bootstrap plugin_hdl
void* plugin_hdl = nullptr;
aclshmemi_bootstrap_handle_t g_boot_handle;
std::shared_ptr<memory_manager> aclshmemi_memory_manager = nullptr;
std::shared_ptr<memory_manager> aclshmemi_host_memory_manager = nullptr;

// Count Used to guard Multi instance scenario
static int g_init_manager_count = 0;
aclshmemi_init_backend* init_manager = nullptr;

// Protect instance context access
static std::mutex g_aclshmem_ctx_mutex;
static shm::transport::UdmaQpConfig g_udma_qp_config{};
static shm::transport::TransportOptions::RdmaQpConfig g_rdma_qp_config{};
static shm::transport::SdmaQpConfig g_sdma_qp_config{};
static bool g_qp_config_frozen = false;

// Instance context used to store global_resources
struct aclshmem_context {
    uint64_t instance_id = 0;

    aclshmem_device_host_state_t state = ACLSHMEM_DEVICE_HOST_STATE_INITIALIZER;
    aclshmem_host_state_t host_state = {nullptr, DEFAULT_TEVENT, DEFAULT_BLOCK_NUM};

    void* bootstrap_plugin_hdl = nullptr;
    aclshmemi_bootstrap_handle_t boot_handle;

    std::shared_ptr<memory_manager> dev_mem_manager = nullptr;
    std::shared_ptr<memory_manager> host_mem_manager = nullptr;
    aclshmemi_exception_report_context_t exception_report_context{};
    aclshmem_prof_pe_t host_profs{};

    explicit aclshmem_context(uint64_t id) : instance_id(id) {}
};

// g_instance_ctx for global use
aclshmem_instance_ctx* g_instance_ctx = new aclshmem_instance_ctx{0, nullptr};

std::map<uint32_t, aclshmem_instance_ctx*> aclshmem_ctx_domain = {{0, g_instance_ctx}};
std::map<uint32_t, aclshmem_context*> aclshmem_resource_domain = {{0, new aclshmem_context(0)}};

namespace {
int32_t query_vector_core_count(uint32_t& aiv_count)
{
    aiv_count = 0;
    int32_t device_id = -1;
    auto ret = aclrtGetDevice(&device_id);
    if (ret != ACL_SUCCESS) {
        SHM_LOG_ERROR("aclrtGetDevice failed when initializing the core sync pool, ret=" << ret);
        return ACLSHMEM_INNER_ERROR;
    }

    int64_t vector_core_num = 0;
    const auto device_info_ret = aclrtGetDeviceInfo(device_id, ACL_DEV_ATTR_VECTOR_CORE_NUM, &vector_core_num);
    if (device_info_ret != ACL_SUCCESS || vector_core_num <= 0) {
        const int64_t device_info_value = vector_core_num;
        int64_t capability_value = 0;
        const auto capability_ret =
            aclGetDeviceCapability(device_id, ACL_DEVICE_INFO_VECTOR_CORE_NUM, &capability_value);
        if (capability_ret != ACL_SUCCESS || capability_value <= 0) {
            SHM_LOG_ERROR(
                "failed to query vector core count for core sync pool, device_id="
                << device_id << ", aclrtGetDeviceInfo_ret=" << device_info_ret << ", aclrtGetDeviceInfo_value="
                << device_info_value << ", aclGetDeviceCapability_ret=" << capability_ret
                << ", aclGetDeviceCapability_value=" << capability_value);
            return ACLSHMEM_INNER_ERROR;
        }
        SHM_LOG_WARN(
            "aclrtGetDeviceInfo did not return a valid vector core count, device_id="
            << device_id << ", ret=" << device_info_ret << ", value=" << device_info_value
            << "; aclGetDeviceCapability succeeded, value=" << capability_value);
        vector_core_num = capability_value;
    }
    if (vector_core_num > static_cast<int64_t>(std::numeric_limits<uint32_t>::max())) {
        SHM_LOG_ERROR(
            "invalid vector core count for core sync pool, device_id=" << device_id
                                                                       << ", vector_core_num=" << vector_core_num);
        return ACLSHMEM_INNER_ERROR;
    }

    aiv_count = static_cast<uint32_t>(vector_core_num);
    SHM_LOG_INFO("initialize core sync pool for " << aiv_count << " AIVs");
    return ACLSHMEM_SUCCESS;
}

bool aclshmemi_parse_port_strict(const std::string& port_str, uint16_t& port)
{
    constexpr int max_port = 65535;
    try {
        size_t parsed_len = 0;
        int port_int = std::stoi(port_str, &parsed_len);
        if (parsed_len != port_str.size() || port_int < 0 || port_int > max_port) {
            return false;
        }
        port = static_cast<uint16_t>(port_int);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
} // namespace

int32_t version_compatible()
{
    int32_t status = ACLSHMEM_SUCCESS;
    return status;
}

int32_t aclshmemi_state_init_attr(aclshmemx_init_attr_t* attributes, uint64_t heap_size)
{
    int32_t status = ACLSHMEM_SUCCESS;
    g_state.mype = attributes->my_pe;
    g_state.npes = attributes->n_pes;
    g_state.heap_size = heap_size;

    aclrtStream stream = nullptr;
    ACLSHMEM_CHECK_RET(aclrtCreateStream(&stream));
    g_state_host.default_stream = stream;
    g_state_host.default_event_id = DEFAULT_TEVENT;
    g_state_host.default_block_num = DEFAULT_BLOCK_NUM;
    return status;
}

bool is_valid_data_op_engine_type(data_op_engine_type_t value)
{
    constexpr uint32_t valid_mask = (static_cast<uint32_t>(ACLSHMEM_DATA_OP_MAX) << 1) - 1;
    uint32_t int_value = static_cast<uint32_t>(value);
    return int_value > 0 && (int_value & ~valid_mask) == 0;
}

bool is_valid_qp_num(uint32_t qp_num, uint32_t max_qp_num)
{
    constexpr uint32_t min_qp_num = 1U;
    return qp_num >= min_qp_num && qp_num <= max_qp_num;
}

bool aclshmemi_user_buffer_heap_engine_supported(data_op_engine_type_t engine)
{
    constexpr uint32_t mte_engine = static_cast<uint32_t>(ACLSHMEM_DATA_OP_MTE);
#if defined(ACLSHMEM_SOC_950)
    constexpr uint32_t supported_engines = mte_engine | static_cast<uint32_t>(ACLSHMEM_DATA_OP_UDMA);
#else
    constexpr uint32_t supported_engines = mte_engine;
#endif
    const uint32_t requested_engines = static_cast<uint32_t>(engine);
    return requested_engines != 0 && (requested_engines & ~supported_engines) == 0;
}

static int32_t check_common_attr(aclshmemx_init_attr_t* attributes)
{
    SHM_LOG_DEBUG(
        "check_attr my_pe=" << attributes->my_pe << " n_pes=" << attributes->n_pes
                            << " local_mem_size=" << attributes->local_mem_size
                            << " shm_init_timeout=" << attributes->option_attr.shm_init_timeout
                            << " control_operation_timeout=" << attributes->option_attr.control_operation_timeout);
    SHM_VALIDATE_RETURN(attributes->my_pe >= 0, "my_pe is less than zero", ACLSHMEM_INVALID_VALUE);
    SHM_VALIDATE_RETURN(attributes->n_pes > 0, "n_pes is less than or equal to zero", ACLSHMEM_INVALID_VALUE);
    SHM_VALIDATE_RETURN(attributes->n_pes <= ACLSHMEM_MAX_PES, "n_pes is too large", ACLSHMEM_INVALID_VALUE);
    SHM_VALIDATE_RETURN(
        attributes->my_pe < attributes->n_pes, "my_pe is greater than or equal to n_pes", ACLSHMEM_INVALID_PARAM);
    SHM_VALIDATE_RETURN(
        attributes->option_attr.shm_init_timeout != 0, "shm_init_timeout is zero", ACLSHMEM_INVALID_VALUE);
    SHM_VALIDATE_RETURN(
        attributes->option_attr.control_operation_timeout != 0, "control_operation_timeout is zero",
        ACLSHMEM_INVALID_VALUE);
    SHM_VALIDATE_RETURN(
        attributes->option_attr.data_op_engine_type > 0, "data_op_engine_type is invalid", ACLSHMEM_INVALID_VALUE);
    SHM_ASSERT_RETURN(
        is_valid_data_op_engine_type(attributes->option_attr.data_op_engine_type), ACLSHMEM_INVALID_VALUE);
    return ACLSHMEM_SUCCESS;
}

int32_t check_attr(aclshmemx_init_attr_t* attributes)
{
    ACLSHMEM_CHECK_RET(check_common_attr(attributes));
    SHM_VALIDATE_RETURN(
        attributes->local_mem_size > 0, "local_mem_size less than or equal to 0", ACLSHMEM_INVALID_VALUE);
    SHM_ASSERT_RETURN(attributes->local_mem_size <= ACLSHMEM_MAX_LOCAL_SIZE, ACLSHMEM_INVALID_VALUE);
    return ACLSHMEM_SUCCESS;
}

static int32_t check_user_buffer_heap_attr(aclshmemx_init_attr_t* attributes)
{
    ACLSHMEM_CHECK_RET(check_common_attr(attributes));
    SHM_ASSERT_RETURN(attributes->local_mem_size <= ACLSHMEM_MAX_LOCAL_SIZE, ACLSHMEM_INVALID_VALUE);
    SHM_ASSERT_RETURN(attributes->local_mem_size % ACLSHMEM_PAGE_SIZE == 0, ACLSHMEM_INVALID_PARAM);
    SHM_ASSERT_RETURN(
        aclshmemi_user_buffer_heap_engine_supported(attributes->option_attr.data_op_engine_type),
        ACLSHMEM_NOT_SUPPORTED);
    return ACLSHMEM_SUCCESS;
}

static bool checked_add_u64(uint64_t lhs, uint64_t rhs, uint64_t* result)
{
    if (result == nullptr || lhs > std::numeric_limits<uint64_t>::max() - rhs) {
        return false;
    }
    *result = lhs + rhs;
    return true;
}

static bool checked_mul_size(size_t lhs, size_t rhs, size_t* result)
{
    if (result == nullptr || (rhs != 0 && lhs > std::numeric_limits<size_t>::max() / rhs)) {
        return false;
    }
    *result = lhs * rhs;
    return true;
}

shm::UserBufferHeapInput::~UserBufferHeapInput()
{
#ifdef HAS_ACLRT_MEM_FABRIC_HANDLE
    for (auto& entry : entries) {
        if (entry.handle_ownership != shm::UserBufferHandleOwnership::RETAINED || entry.mem_handle == nullptr) {
            continue;
        }
        const auto ret = aclrtFreePhysical(entry.mem_handle);
        if (ret != ACL_SUCCESS) {
            SHM_LOG_WARN("Failed to release retained buffer allocation handle, ret=" << ret);
        }
        entry.mem_handle = nullptr;
    }
#endif
}

#ifdef HAS_ACLRT_MEM_FABRIC_HANDLE
static bool reserved_fields_are_zero(const uint64_t* reserved, size_t count)
{
    return reserved != nullptr && std::all_of(reserved, reserved + count, [](uint64_t value) { return value == 0; });
}

static int32_t validate_and_normalize_buffer_property(
    int32_t current_device, size_t index, bool validate_device_ownership, aclrtPhysicalMemProp* property)
{
    SHM_ASSERT_RETURN(property != nullptr, ACLSHMEM_INVALID_PARAM);
    if (property->handleType != ACL_MEM_HANDLE_TYPE_NONE ||
        property->allocationType != ACL_MEM_ALLOCATION_TYPE_PINNED ||
        property->location.type != ACL_MEM_LOCATION_TYPE_DEVICE ||
        (property->memAttr != ACL_HBM_MEM_HUGE && property->memAttr != ACL_HBM_MEM_HUGE1G)) {
        SHM_LOG_ERROR(
            "Unsupported physical allocation properties for buffer "
            << index << ": handleType=" << static_cast<int>(property->handleType)
            << ", allocationType=" << static_cast<int>(property->allocationType) << ", locationType="
            << static_cast<int>(property->location.type) << ", locationId=" << property->location.id
            << ", currentDevice=" << current_device << ", memAttr=" << static_cast<int>(property->memAttr));
        return ACLSHMEM_NOT_SUPPORTED;
    }
    if (validate_device_ownership) {
        int32_t current_logic_device = -1;
        const auto map_status = shm::DlAclApi::RtGetLogicDevIdByUserDevId(current_device, &current_logic_device);
        if (map_status != ACLSHMEM_SUCCESS || current_logic_device < 0) {
            SHM_LOG_ERROR(
                "Failed to map current user device " << current_device
                                                     << " to a Driver logic device, ret=" << map_status);
            return ACLSHMEM_NOT_SUPPORTED;
        }
        if (property->location.id != current_logic_device) {
            SHM_LOG_ERROR(
                "Buffer " << index << " belongs to Driver logic device " << property->location.id
                          << ", currentUserDevice=" << current_device
                          << ", currentLogicDevice=" << current_logic_device);
            return ACLSHMEM_NOT_SUPPORTED;
        }
        // Subsequent ACL APIs take a user-visible device ID and perform their own user-to-logic conversion.
        property->location.id = current_device;
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t query_buffer_property(
    aclrtDrvMemHandle mem_handle, int32_t current_device, size_t index, aclrtPhysicalMemProp* property)
{
    SHM_ASSERT_RETURN(mem_handle != nullptr && property != nullptr, ACLSHMEM_INVALID_PARAM);
    auto acl_status = shm::DlAclApi::AclrtMemGetAllocationPropertiesFromHandle(mem_handle, property);
    if (acl_status == ACLSHMEM_UNDER_API_UNLOAD || acl_status == ACL_ERROR_RT_FEATURE_NOT_SUPPORT) {
        property->handleType = ACL_MEM_HANDLE_TYPE_NONE;
        property->allocationType = ACL_MEM_ALLOCATION_TYPE_PINNED;
        property->location.type = ACL_MEM_LOCATION_TYPE_DEVICE;
        property->location.id = current_device;
        property->memAttr = ACL_HBM_MEM_HUGE;
        property->reserve = 0;
        SHM_LOG_WARN(
            "Physical allocation property query is not supported; deferring buffer "
            << index << " handle validation to export/map");
    } else if (acl_status != ACL_SUCCESS) {
        SHM_LOG_ERROR(
            "aclrtMemGetAllocationPropertiesFromHandle failed for buffer " << index << ", ret=" << acl_status);
        return ACLSHMEM_NOT_SUPPORTED;
    }
    return validate_and_normalize_buffer_property(current_device, index, acl_status == ACL_SUCCESS, property);
}

static int32_t validate_user_buffer_metadata(size_t buffer_count, int32_t npes)
{
    if (buffer_count == 0 || buffer_count > shm::kMaxBuffers || npes <= 0) {
        return ACLSHMEM_INVALID_PARAM;
    }

    size_t metadata_count = 0;
    size_t metadata_bytes = 0;
    if (buffer_count == std::numeric_limits<size_t>::max() ||
        !checked_mul_size(buffer_count + 1, static_cast<size_t>(npes), &metadata_count) ||
        !checked_mul_size(metadata_count, sizeof(hybm_exchange_info), &metadata_bytes)) {
        return ACLSHMEM_INVALID_VALUE;
    }
    if (buffer_count + 1 > std::numeric_limits<uint32_t>::max() || metadata_bytes > shm::kMaxBufferMetadataBytes) {
        return ACLSHMEM_INVALID_VALUE;
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t validate_buffer_descriptor(
    const aclshmemx_buffer_desc_t& source, const shm::UserBufferHeapInput& input, size_t index)
{
    if (source.addr == nullptr || source.size == 0 ||
        !reserved_fields_are_zero(source.reserved, sizeof(source.reserved) / sizeof(source.reserved[0])) ||
        (source.optional_attr != nullptr &&
         !reserved_fields_are_zero(
             source.optional_attr->reserved,
             sizeof(source.optional_attr->reserved) / sizeof(source.optional_attr->reserved[0])))) {
        return ACLSHMEM_INVALID_PARAM;
    }

    const uintptr_t source_base = reinterpret_cast<uintptr_t>(source.addr);
    if (source.size > static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max() - source_base)) {
        return ACLSHMEM_INVALID_VALUE;
    }
    const uintptr_t source_end = source_base + static_cast<uintptr_t>(source.size);
    for (size_t previous_index = 0; previous_index < index; ++previous_index) {
        const auto& previous = input.entries[previous_index];
        const uintptr_t previous_base = reinterpret_cast<uintptr_t>(previous.source_base);
        const uintptr_t previous_end = previous_base + static_cast<uintptr_t>(previous.size);
        if (source_base < previous_end && previous_base < source_end) {
            return ACLSHMEM_INVALID_PARAM;
        }
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t acquire_buffer_handle(
    const aclshmemx_buffer_desc_t& source, int32_t current_device, size_t index, shm::UserBufferHeapInput* input)
{
    SHM_ASSERT_RETURN(input != nullptr && index < input->entries.size(), ACLSHMEM_INVALID_PARAM);
    auto& destination = input->entries[index];
    if (source.optional_attr != nullptr && source.optional_attr->mem_handle != nullptr) {
        destination.mem_handle = static_cast<aclrtDrvMemHandle>(source.optional_attr->mem_handle);
        destination.handle_ownership = shm::UserBufferHandleOwnership::CALLER;
    } else {
        const auto status = aclrtMemRetainAllocationHandle(source.addr, &destination.mem_handle);
        if (status != ACL_SUCCESS || destination.mem_handle == nullptr) {
            SHM_LOG_ERROR("aclrtMemRetainAllocationHandle failed for buffer " << index << ", ret=" << status);
            return ACLSHMEM_INVALID_PARAM;
        }
        destination.handle_ownership = shm::UserBufferHandleOwnership::RETAINED;
    }
    for (size_t previous_index = 0; previous_index < index; ++previous_index) {
        if (input->entries[previous_index].mem_handle == destination.mem_handle) {
            return ACLSHMEM_INVALID_PARAM;
        }
    }
    return query_buffer_property(destination.mem_handle, current_device, index, &destination.property);
}

static int32_t validate_buffer_mapping(const aclshmemx_buffer_desc_t& source, size_t index, shm::UserBufferEntry* entry)
{
    SHM_ASSERT_RETURN(entry != nullptr, ACLSHMEM_INVALID_PARAM);
    size_t granularity = 0;
    const auto status =
        aclrtMemGetAllocationGranularity(&entry->property, ACL_RT_MEM_ALLOC_GRANULARITY_MINIMUM, &granularity);
    if (status != ACL_SUCCESS || granularity == 0) {
        SHM_LOG_ERROR(
            "aclrtMemGetAllocationGranularity failed for buffer " << index << ", ret=" << status
                                                                  << ", granularity=" << granularity);
        return ACLSHMEM_NOT_SUPPORTED;
    }

    const uintptr_t source_base = reinterpret_cast<uintptr_t>(source.addr);
    if (source_base % granularity != 0 || entry->size % granularity != 0 || entry->segment_offset % granularity != 0) {
        SHM_LOG_ERROR(
            "Unaligned buffer " << index << ": sourceBase=" << source_base << ", size=" << entry->size
                                << ", segmentOffset=" << entry->segment_offset << ", granularity=" << granularity);
        return ACLSHMEM_INVALID_PARAM;
    }

    const auto* optional_attr = source.optional_attr;
    entry->has_fabric_handle = optional_attr != nullptr && optional_attr->fabric_handle != nullptr;
    if (entry->has_fabric_handle) {
        std::memcpy(&entry->fabric_handle, optional_attr->fabric_handle, sizeof(entry->fabric_handle));
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t calculate_user_buffer_layout(
    uint64_t external_bytes, uint64_t local_mem_size, uint32_t buffer_count, shm::UserBufferHeapLayoutHeader* header)
{
    SHM_ASSERT_RETURN(header != nullptr, ACLSHMEM_INVALID_PARAM);
    uint64_t user_bytes = 0;
    uint64_t owned_bytes = 0;
    uint64_t heap_size = 0;
    if (!checked_add_u64(external_bytes, local_mem_size, &user_bytes) || user_bytes > ACLSHMEM_MAX_LOCAL_SIZE ||
        !checked_add_u64(local_mem_size, ACLSHMEM_EXTRA_SIZE, &owned_bytes) ||
        !checked_add_u64(external_bytes, owned_bytes, &heap_size)) {
        return ACLSHMEM_INVALID_VALUE;
    }
    if (heap_size % ACLSHMEM_PAGE_SIZE != 0) {
        return ACLSHMEM_INVALID_PARAM;
    }

    header->buffer_count = buffer_count;
    header->external_bytes = external_bytes;
    header->allocatable_bytes = local_mem_size;
    header->heap_size = heap_size;
    return ACLSHMEM_SUCCESS;
}

static int32_t build_local_layout_header(
    const aclshmemx_buffer_desc_t* buffers, size_t buffer_count, const aclshmemx_init_attr_t* attributes,
    std::unique_ptr<shm::UserBufferHeapInput>* input, shm::UserBufferHeapLayoutHeader* header)
{
    SHM_ASSERT_RETURN(attributes != nullptr && input != nullptr && header != nullptr, ACLSHMEM_INVALID_PARAM);
    *header = {};
    if (buffers == nullptr) {
        header->local_status = ACLSHMEM_INVALID_PARAM;
        return header->local_status;
    }
    header->local_status = validate_user_buffer_metadata(buffer_count, attributes->n_pes);
    if (header->local_status != ACLSHMEM_SUCCESS) {
        return header->local_status;
    }

    std::unique_ptr<shm::UserBufferHeapInput> local_input;
    try {
        local_input = std::make_unique<shm::UserBufferHeapInput>();
        local_input->entries.resize(buffer_count);
    } catch (const std::bad_alloc&) {
        header->local_status = ACLSHMEM_MALLOC_FAILED;
        return header->local_status;
    }

    int32_t current_device = -1;
    auto acl_status = aclrtGetDevice(&current_device);
    if (acl_status != ACL_SUCCESS) {
        SHM_LOG_ERROR("aclrtGetDevice failed while normalizing user buffer heap, ret=" << acl_status);
        header->local_status = ACLSHMEM_NOT_SUPPORTED;
        return header->local_status;
    }

    uint64_t external_bytes = 0;
    for (size_t i = 0; i < buffer_count; ++i) {
        const auto& src = buffers[i];
        header->local_status = validate_buffer_descriptor(src, *local_input, i);
        if (header->local_status != ACLSHMEM_SUCCESS) {
            return header->local_status;
        }
        auto& dst = local_input->entries[i];
        dst.source_base = src.addr;
        dst.size = src.size;
        dst.segment_offset = external_bytes;
        header->local_status = acquire_buffer_handle(src, current_device, i, local_input.get());
        if (header->local_status == ACLSHMEM_SUCCESS) {
            header->local_status = validate_buffer_mapping(src, i, &dst);
        }
        if (header->local_status != ACLSHMEM_SUCCESS) {
            return header->local_status;
        }
        if (!checked_add_u64(external_bytes, src.size, &external_bytes)) {
            header->local_status = ACLSHMEM_INVALID_VALUE;
            return header->local_status;
        }
    }

    header->local_status = calculate_user_buffer_layout(
        external_bytes, attributes->local_mem_size, static_cast<uint32_t>(buffer_count), header);
    if (header->local_status != ACLSHMEM_SUCCESS) {
        return header->local_status;
    }

    *input = std::move(local_input);
    return ACLSHMEM_SUCCESS;
}

static int32_t validate_layout_headers(const std::vector<shm::UserBufferHeapLayoutHeader>& headers, int32_t npes)
{
    SHM_ASSERT_RETURN(npes > 0 && headers.size() == static_cast<size_t>(npes), ACLSHMEM_INVALID_PARAM);
    for (int32_t pe = 0; pe < npes; ++pe) {
        if (headers[pe].local_status != ACLSHMEM_SUCCESS) {
            return headers[pe].local_status;
        }
    }
    const auto& expected = headers[0];
    for (int32_t pe = 1; pe < npes; ++pe) {
        const auto& current = headers[pe];
        if (current.buffer_count != expected.buffer_count || current.external_bytes != expected.external_bytes ||
            current.allocatable_bytes != expected.allocatable_bytes || current.heap_size != expected.heap_size) {
            return ACLSHMEM_INVALID_PARAM;
        }
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t validate_size_vectors(
    const shm::UserBufferHeapInput& input, int32_t npes, aclshmemi_bootstrap_handle_t* boot_handle)
{
    SHM_ASSERT_RETURN(boot_handle != nullptr && boot_handle->allgather != nullptr, ACLSHMEM_INVALID_PARAM);
    SHM_ASSERT_RETURN(!input.entries.empty(), ACLSHMEM_INVALID_PARAM);
    size_t local_bytes = 0;
    size_t gathered_count = 0;
    if (!checked_mul_size(input.entries.size(), sizeof(uint64_t), &local_bytes) ||
        local_bytes > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        !checked_mul_size(input.entries.size(), static_cast<size_t>(npes), &gathered_count)) {
        return ACLSHMEM_INVALID_VALUE;
    }
    std::vector<uint64_t> local_sizes;
    std::vector<uint64_t> all_sizes;
    try {
        local_sizes.resize(input.entries.size());
        all_sizes.resize(gathered_count);
    } catch (const std::bad_alloc&) {
        return ACLSHMEM_MALLOC_FAILED;
    }
    for (size_t i = 0; i < input.entries.size(); ++i) {
        local_sizes[i] = input.entries[i].size;
    }
    ACLSHMEM_CHECK_RET(
        boot_handle->allgather(local_sizes.data(), all_sizes.data(), static_cast<int>(local_bytes), boot_handle));
    for (int32_t pe = 0; pe < npes; ++pe) {
        for (size_t i = 0; i < local_sizes.size(); ++i) {
            if (all_sizes[static_cast<size_t>(pe) * local_sizes.size() + i] != local_sizes[i]) {
                return ACLSHMEM_INVALID_PARAM;
            }
        }
    }
    return ACLSHMEM_SUCCESS;
}
#endif

int32_t aclshmemi_collective_status_gate(int32_t local_status, int32_t npes, aclshmemi_bootstrap_handle_t* boot_handle)
{
    SHM_ASSERT_RETURN(boot_handle != nullptr, ACLSHMEM_INVALID_PARAM);
    SHM_ASSERT_RETURN(boot_handle->allgather != nullptr, ACLSHMEM_INVALID_PARAM);
    SHM_ASSERT_RETURN(npes > 0 && npes <= ACLSHMEM_MAX_PES, ACLSHMEM_INVALID_PARAM);

    std::vector<int32_t> all_status;
    try {
        all_status.resize(static_cast<size_t>(npes));
    } catch (const std::bad_alloc&) {
        return ACLSHMEM_MALLOC_FAILED;
    }
    int32_t status =
        boot_handle->allgather(&local_status, all_status.data(), static_cast<int>(sizeof(local_status)), boot_handle);
    ACLSHMEM_CHECK_RET(status);

    for (int32_t pe = 0; pe < npes; ++pe) {
        if (all_status[pe] != ACLSHMEM_SUCCESS) {
            return all_status[pe];
        }
    }
    return ACLSHMEM_SUCCESS;
}

int32_t aclshmemi_control_barrier_all() { return init_manager->aclshmemi_control_barrier_all(); }

int32_t is_alloc_size_symmetric(size_t size) { return init_manager->is_alloc_size_symmetric(size); }

int32_t update_device_state()
{
    return init_manager->update_device_state((void*)&g_state, sizeof(aclshmem_device_host_state_t));
}

int32_t aclshmemx_init_status(void)
{
    if (!g_state.is_aclshmem_created)
        return ACLSHMEM_STATUS_NOT_INITIALIZED;
    else if (!g_state.is_aclshmem_initialized)
        return ACLSHMEM_STATUS_SHM_CREATED;
    else if (g_state.is_aclshmem_initialized)
        return ACLSHMEM_STATUS_IS_INITIALIZED;
    else
        return ACLSHMEM_STATUS_INVALID;
}

int aclshmemx_set_attr_uniqueid_args(
    int my_pe, int n_pes, int64_t local_mem_size, aclshmemx_uniqueid_t* uid, aclshmemx_init_attr_t* aclshmem_attr)
{
    /* Save to uid_args */
    SHM_ASSERT_RETURN(local_mem_size <= ACLSHMEM_MAX_LOCAL_SIZE, ACLSHMEM_INVALID_VALUE);
    SHM_ASSERT_RETURN(n_pes <= ACLSHMEM_MAX_PES, ACLSHMEM_INVALID_VALUE);
    SHM_ASSERT_RETURN(my_pe < ACLSHMEM_MAX_PES, ACLSHMEM_INVALID_VALUE);
    aclshmemi_bootstrap_uid_state_t* uid_args = (aclshmemi_bootstrap_uid_state_t*)(uid);
    void* comm_args = reinterpret_cast<void*>(uid_args);
    aclshmem_attr->comm_args = comm_args;
    aclshmem_attr->my_pe = my_pe;
    aclshmem_attr->n_pes = n_pes;
    aclshmem_attr->local_mem_size = local_mem_size;

    return ACLSHMEM_SUCCESS;
}

int aclshmemx_set_qp_num(data_op_engine_type_t engine, uint32_t qp_num)
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (g_qp_config_frozen) {
        SHM_LOG_ERROR("QP configuration cannot be changed while an ACLSHMEM instance is initialized.");
        return ACLSHMEM_NOT_SUPPORTED;
    }
    switch (engine) {
        case ACLSHMEM_DATA_OP_SDMA: {
            int device_id = -1;
            int64_t vector_core_num = 0;
            (void)aclrtGetDevice(&device_id);
            auto query_ret = aclrtGetDeviceInfo(device_id, ACL_DEV_ATTR_VECTOR_CORE_NUM, &vector_core_num);
            if (query_ret != ACL_SUCCESS || vector_core_num <= 0) {
                query_ret = aclGetDeviceCapability(device_id, ACL_DEVICE_INFO_VECTOR_CORE_NUM, &vector_core_num);
            }
            const uint32_t max_qp_num =
                (query_ret == ACL_SUCCESS && vector_core_num > 0) ?
                    std::min<uint32_t>(static_cast<uint32_t>(vector_core_num), ACLSHMEM_MAX_AIV_PER_NPU) :
                    ACLSHMEM_MAX_AIV_PER_NPU;
            if (!is_valid_qp_num(qp_num, max_qp_num)) {
                SHM_LOG_ERROR("invalid SDMA qp num: " << qp_num << ", supported range is [1, " << max_qp_num << "]");
                return ACLSHMEM_INVALID_VALUE;
            }
            g_sdma_qp_config.qpNum = qp_num;
            break;
        }
        case ACLSHMEM_DATA_OP_ROCE:
#if !defined(ACLSHMEM_RDMA_V2_SUPPORT)
            SHM_LOG_WARN("ROCE QP configuration requires the Ascend950 RDMA v2 backend");
            return ACLSHMEM_NOT_SUPPORTED;
#else
            if (!is_valid_qp_num(qp_num, ACLSHMEM_MAX_QP_NUM)) {
                SHM_LOG_ERROR("invalid ROCE qp num: " << qp_num);
                return ACLSHMEM_INVALID_VALUE;
            }
            g_rdma_qp_config.qpNum = qp_num;
            break;
#endif
        case ACLSHMEM_DATA_OP_UDMA:
            if (!is_valid_qp_num(qp_num, ACLSHMEM_MAX_QP_NUM)) {
                SHM_LOG_ERROR("invalid UDMA qp num: " << qp_num);
                return ACLSHMEM_INVALID_VALUE;
            }
            if (!g_udma_qp_config.netLayerPolicy.empty() && g_udma_qp_config.netLayerPolicy.rbegin()->first >= qp_num) {
                SHM_LOG_ERROR(
                    "UDMA qp num " << qp_num << " would invalidate configured QP index "
                                   << g_udma_qp_config.netLayerPolicy.rbegin()->first);
                return ACLSHMEM_INVALID_VALUE;
            }
            g_udma_qp_config.qpNum = qp_num;
            break;
        default:
            SHM_LOG_WARN("QP count configuration does not support engine = " << engine);
            return ACLSHMEM_NOT_SUPPORTED;
    }
    SHM_LOG_INFO("set qp num success, engine=" << engine << ", qp_num=" << qp_num);
    return ACLSHMEM_SUCCESS;
}

int aclshmemx_get_qp_num(data_op_engine_type_t engine, uint32_t* qp_num)
{
    if (qp_num == nullptr) {
        SHM_LOG_ERROR("Failed to get QP count: qp_num output pointer is null, engine=" << engine);
        return ACLSHMEM_INVALID_VALUE;
    }
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    switch (engine) {
        case ACLSHMEM_DATA_OP_SDMA:
            *qp_num = g_sdma_qp_config.qpNum;
            return ACLSHMEM_SUCCESS;
        case ACLSHMEM_DATA_OP_ROCE:
#if !defined(ACLSHMEM_RDMA_V2_SUPPORT)
            SHM_LOG_ERROR("Failed to get QP count: ROCE V2 support is not enabled.");
            return ACLSHMEM_NOT_SUPPORTED;
#else
            *qp_num = g_rdma_qp_config.qpNum;
            return ACLSHMEM_SUCCESS;
#endif
        case ACLSHMEM_DATA_OP_UDMA:
            *qp_num = g_udma_qp_config.qpNum;
            return ACLSHMEM_SUCCESS;
        default:
            SHM_LOG_ERROR("Failed to get QP count: unsupported engine=" << engine);
            return ACLSHMEM_NOT_SUPPORTED;
    }
}

int aclshmemx_set_qp_net_layer(data_op_engine_type_t engine, uint32_t qp_idx, aclshmemx_net_layer_t net_layer)
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (engine != ACLSHMEM_DATA_OP_UDMA) {
        SHM_LOG_ERROR("QP netLayer policy only supports the UDMA engine, engine=" << engine);
        return ACLSHMEM_NOT_SUPPORTED;
    }
    if (!g_udma_qp_config.multiLinkEnabled) {
        SHM_LOG_ERROR(
            "UDMA QP netLayer policy requires multi-link support; rebuild with ACLSHMEM_MULTI_LINK_SUPPORT=ON.");
        return ACLSHMEM_NOT_SUPPORTED;
    }
    if (g_qp_config_frozen) {
        SHM_LOG_ERROR("UDMA QP netLayer policy cannot be changed while an ACLSHMEM instance is initialized.");
        return ACLSHMEM_NOT_SUPPORTED;
    }
    if (qp_idx >= g_udma_qp_config.qpNum) {
        SHM_LOG_ERROR("invalid UDMA QP index " << qp_idx << ", qp_num=" << g_udma_qp_config.qpNum);
        return ACLSHMEM_INVALID_VALUE;
    }
    const uint32_t layer = static_cast<uint32_t>(net_layer);
    if (net_layer == ACLSHMEM_NET_LAYER_DEFAULT) {
        g_udma_qp_config.netLayerPolicy.erase(qp_idx);
        return ACLSHMEM_SUCCESS;
    }
    if (layer > static_cast<uint32_t>(ACLSHMEM_NET_LAYER_3)) {
        SHM_LOG_ERROR("Invalid UDMA netLayer value " << layer << " for QP index " << qp_idx);
        return ACLSHMEM_INVALID_VALUE;
    }
    if (layer > static_cast<uint32_t>(ACLSHMEM_NET_LAYER_1)) {
        SHM_LOG_ERROR("UDMA QP netLayer " << layer << " is defined but not supported in this release.");
        return ACLSHMEM_NOT_SUPPORTED;
    }
    g_udma_qp_config.netLayerPolicy[qp_idx] = layer;
    return ACLSHMEM_SUCCESS;
}

int aclshmemx_get_qp_net_layer(
    data_op_engine_type_t engine, int peer, uint32_t qp_idx, aclshmemx_net_layer_t* net_layer)
{
    if (net_layer == nullptr) {
        SHM_LOG_ERROR(
            "Failed to get QP netLayer: net_layer output pointer is null, engine=" << engine << ", peer=" << peer
                                                                                   << ", qp_idx=" << qp_idx);
        return ACLSHMEM_INVALID_VALUE;
    }
    if (engine != ACLSHMEM_DATA_OP_UDMA) {
        SHM_LOG_ERROR("Failed to get QP netLayer: unsupported engine=" << engine);
        return ACLSHMEM_NOT_SUPPORTED;
    }
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (init_manager == nullptr || g_instance_ctx == nullptr) {
        SHM_LOG_ERROR("Failed to get QP netLayer: ACLSHMEM is not initialized.");
        return ACLSHMEM_NOT_INITED;
    }
    if (peer < 0 || peer >= g_state.npes) {
        SHM_LOG_ERROR(
            "Failed to get QP netLayer: peer " << peer << " is outside [0, " << g_state.npes << "), qp_idx=" << qp_idx);
        return ACLSHMEM_INVALID_VALUE;
    }
    uint32_t resolved_layer = 0;
    const int ret =
        init_manager->get_qp_net_layer(g_instance_ctx->id, engine, static_cast<uint32_t>(peer), qp_idx, resolved_layer);
    if (ret != ACLSHMEM_SUCCESS) {
        SHM_LOG_ERROR(
            "Failed to get QP netLayer from transport manager, instance_id="
            << g_instance_ctx->id << ", peer=" << peer << ", qp_idx=" << qp_idx << ", ret=" << ret);
        return ret;
    }
    *net_layer = static_cast<aclshmemx_net_layer_t>(resolved_layer);
    return ACLSHMEM_SUCCESS;
}

static bool check_support_d2h()
{
#ifdef USE_ACLRT_MEM_FABRIC_HANDLE
    return true;
#else
    return false;
#endif
}

int32_t aclshmemi_signal_finalize()
{
    if (g_state.signal_addr != 0) {
        aclshmem_free(reinterpret_cast<void*>(g_state.signal_addr));
        g_state.signal_addr = 0;
    }
    return ACLSHMEM_SUCCESS;
}

int32_t aclshmemi_signal_init()
{
    constexpr size_t align_size = 512;
    g_state.signal_addr = (uint64_t)aclshmem_malloc(align_size);
    if (g_state.signal_addr == 0) {
        SHM_LOG_ERROR("malloc signal failed.");
        return ACLSHMEM_INNER_ERROR;
    }
    auto ret = aclrtMemset((void*)g_state.signal_addr, ACLSHMEM_SIGNAL_SIZE, 0, ACLSHMEM_SIGNAL_SIZE);
    if (ret != 0) {
        aclshmemi_signal_finalize();
        SHM_LOG_ERROR("memset signal failed. ret=" << ret);
        return ACLSHMEM_INNER_ERROR;
    }
    return ACLSHMEM_SUCCESS;
}

static int32_t aclshmemi_instance_port_selection(aclshmemx_init_attr_t* attributes)
{
    // 1. Get Port Range From Environment
    const char* env_port_range = std::getenv("SHMEM_INSTANCE_PORT_RANGE");
    if (env_port_range == nullptr) {
        SHM_LOG_ERROR("The environment variable SHMEM_INSTANCE_PORT_RANGE is not set.");
        return ACLSHMEM_INVALID_VALUE;
    }

    std::string env_port_range_str(env_port_range);
    std::size_t env_pos = env_port_range_str.find(':');
    if (env_pos == std::string::npos) {
        SHM_LOG_ERROR("SHMEM_INSTANCE_PORT_RANGE format should be start_port:end_port");
        return ACLSHMEM_INVALID_VALUE;
    }

    uint16_t start_port;
    uint16_t end_port;
    std::string start_port_str = env_port_range_str.substr(0, env_pos);
    std::string end_port_str = env_port_range_str.substr(env_pos + 1, env_port_range_str.size());
    if (!aclshmemi_parse_port_strict(start_port_str, start_port)) {
        SHM_LOG_ERROR("Invalid start_port value: " << start_port_str << ", must be in range [0, 65535]");
        return ACLSHMEM_INVALID_VALUE;
    }
    if (!aclshmemi_parse_port_strict(end_port_str, end_port)) {
        SHM_LOG_ERROR("Invalid end_port value: " << end_port_str << ", must be in range [0, 65535]");
        return ACLSHMEM_INVALID_VALUE;
    }
    if (end_port < start_port) {
        SHM_LOG_ERROR("SHMEM_INSTANCE_PORT_RANGE: start_port " << start_port << " exceeds end_port " << end_port);
        return ACLSHMEM_INVALID_VALUE;
    }

    // 2. Check if instance_id is valid
    int max_instance_num = end_port - start_port;
    uint64_t instance_id = attributes->instance_id;
    if (instance_id > static_cast<uint64_t>(max_instance_num)) {
        SHM_LOG_ERROR(
            "instance_id " << instance_id << " exceeds max_instance_num " << max_instance_num << " in default mode");
        return ACLSHMEM_INVALID_VALUE;
    }
    uint16_t port = start_port + static_cast<uint16_t>(instance_id);

    // 3. If ip_port is null, return
    if (attributes->ip_port[0] == '\0') {
        SHM_LOG_ERROR("init with my_rank:" << attributes->my_pe << " ip_port is nullptr!");
        return ACLSHMEM_INVALID_VALUE;
    }

    // 4. replace ip_port in attributes
    std::string ip_port_str(attributes->ip_port);
    std::size_t pos;
    if (ip_port_str.find("tcp6://") == 0) {
        std::size_t bracketEnd = ip_port_str.find(']');
        if (bracketEnd == std::string::npos || bracketEnd + 2 >= ip_port_str.size() ||
            ip_port_str[bracketEnd + 1] != ':') {
            SHM_LOG_ERROR("ip_port format should be tcp6://[ipv6_addr]:port");
            return ACLSHMEM_INVALID_VALUE;
        }
        pos = bracketEnd + 1;
    } else {
        pos = ip_port_str.find(':', ip_port_str.find(':') + 1);
    }
    if (pos == std::string::npos) {
        SHM_LOG_ERROR("ip_port format should be ip:port");
        return ACLSHMEM_INVALID_VALUE;
    }

    uint16_t input_port;
    std::string input_port_str = ip_port_str.substr(pos + 1, ip_port_str.size());
    if (!aclshmemi_parse_port_strict(input_port_str, input_port)) {
        SHM_LOG_ERROR("Invalid input_port value: " << input_port_str << ", must be in range [0, 65535]");
        return ACLSHMEM_INVALID_VALUE;
    }
    if (input_port != 0) {
        SHM_LOG_ERROR("input_port must be 0 in default mode, but input_port is " << input_port);
        return ACLSHMEM_INVALID_VALUE;
    }

    std::string instance_ipport = ip_port_str.substr(0, pos) + ":" + std::to_string(port);
    if (instance_ipport.empty()) {
        SHM_LOG_ERROR("my_rank:" << attributes->my_pe << " instance ipport is nullptr!");
        return ACLSHMEM_INVALID_VALUE;
    }

    // 5. Write port in attributes
    size_t ipport_len = std::min(instance_ipport.size(), static_cast<std::size_t>(ACLSHMEM_MAX_IP_PORT_LEN - 1));
    std::copy_n(instance_ipport.c_str(), ipport_len, attributes->ip_port);
    attributes->ip_port[ipport_len] = '\0';

    return ACLSHMEM_SUCCESS;
}

static int aclshmemi_instance_ctx_create(aclshmemx_init_attr_t* attributes)
{
    uint64_t instance_id = attributes->instance_id;
    if (instance_id == 0) {
        // Do Nothing.
        return ACLSHMEM_SUCCESS;
    }

    // Check if Repeat
    if (aclshmem_ctx_domain.find(instance_id) != aclshmem_ctx_domain.end() ||
        aclshmem_resource_domain.find(instance_id) != aclshmem_resource_domain.end()) {
        SHM_LOG_WARN("Instance " << instance_id << " already exists ! Please don't repeat create !");
        return ACLSHMEM_SUCCESS;
    }

    // aclshmem_instance port selection, if init with uid, skip
    if (attributes->ip_port[0] != '\0' || attributes->comm_args == nullptr) {
        ACLSHMEM_CHECK_RET(aclshmemi_instance_port_selection(attributes));
    }

    // Create aclshmem_instance_ctx
    aclshmem_instance_ctx* aclshmem_ctx = new (std::nothrow) aclshmem_instance_ctx{attributes->instance_id, nullptr};
    if (aclshmem_ctx == nullptr) {
        SHM_LOG_ERROR("Failed to allocate memory for aclshmem_instance_ctx.");
        return ACLSHMEM_INNER_ERROR;
    }
    aclshmem_ctx_domain[attributes->instance_id] = aclshmem_ctx;

    aclshmem_context* aclshmem_resource_ctx = new (std::nothrow) aclshmem_context(attributes->instance_id);
    if (aclshmem_resource_ctx == nullptr) {
        SHM_LOG_ERROR("Failed to allocate memory for aclshmem_context.");
        aclshmem_ctx_domain.erase(attributes->instance_id);
        delete aclshmem_ctx;
        return ACLSHMEM_INNER_ERROR;
    }
    aclshmem_resource_domain[attributes->instance_id] = aclshmem_resource_ctx;

    SHM_LOG_INFO(
        "PE: " << attributes->my_pe << " USING SHMEM Multi-Instance Mode ! Now Ctx set to Instance " << instance_id
               << " !");
    return ACLSHMEM_SUCCESS;
}

static int aclshmemi_instance_ctx_destroy(uint64_t instance_id)
{
    if (instance_id == 0) {
        // Do Nothing.
        return ACLSHMEM_SUCCESS;
    }

    // if destroy an active context
    if (instance_id == g_instance_ctx->id) {
        // Set active context to Instance 0
        aclshmemx_instance_ctx_set_impl(0);
    }

    // Release Resource
    auto it_resource = aclshmem_resource_domain.find(instance_id);
    if (it_resource == aclshmem_resource_domain.end()) {
        SHM_LOG_ERROR("Instance " << instance_id << " not exists! Illegal Context Destroy!");
        return ACLSHMEM_INNER_ERROR;
    }
    delete it_resource->second;
    aclshmem_resource_domain.erase(it_resource);

    auto it_ctx = aclshmem_ctx_domain.find(instance_id);
    if (it_ctx == aclshmem_ctx_domain.end()) {
        SHM_LOG_ERROR("Instance " << instance_id << " not exists! Illegal Context Destroy!");
        return ACLSHMEM_INNER_ERROR;
    }
    delete it_ctx->second;
    aclshmem_ctx_domain.erase(it_ctx);

    return ACLSHMEM_SUCCESS;
}

aclshmem_instance_ctx* aclshmemx_instance_ctx_get()
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    return g_instance_ctx;
}

int aclshmemx_instance_ctx_set(uint64_t instance_id)
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    return aclshmemx_instance_ctx_set_impl(instance_id);
}

// Inner Context set Interface, No Lock.
int aclshmemx_instance_ctx_set_impl(uint64_t instance_id)
{
    if (aclshmem_ctx_domain.find(instance_id) != aclshmem_ctx_domain.end()) {
        aclshmem_instance_ctx* new_ctx = aclshmem_ctx_domain[instance_id];
        aclshmem_context* new_context = aclshmem_resource_domain[instance_id];
        aclshmem_context* current_context = aclshmem_resource_domain[g_instance_ctx->id];

        // Global_vars write back
        current_context->state = g_state;
        current_context->host_state = g_state_host;
        current_context->bootstrap_plugin_hdl = plugin_hdl;
        current_context->boot_handle = g_boot_handle;
        current_context->dev_mem_manager = aclshmemi_memory_manager;
        current_context->host_mem_manager = aclshmemi_host_memory_manager;
        aclshmemi_exception_report_save_context(current_context->exception_report_context);

        // Set Global_vars by new_context
        g_state = new_context->state;
        g_state_host = new_context->host_state;
        plugin_hdl = new_context->bootstrap_plugin_hdl;
        g_boot_handle = new_context->boot_handle;
        aclshmemi_memory_manager = new_context->dev_mem_manager;
        aclshmemi_host_memory_manager = new_context->host_mem_manager;
        aclshmemi_exception_report_restore_context(new_context->exception_report_context);

        // Set New aclshmem context
        g_instance_ctx = aclshmem_ctx_domain[instance_id];

        return ACLSHMEM_SUCCESS;
    }
    SHM_LOG_ERROR("Context Set failed ! Can't find instance " << instance_id << " !");
    return ACLSHMEM_INNER_ERROR;
}

enum class InitHeapMode : uint8_t {
    DEFAULT_HEAP,
    USER_BUFFER_HEAP,
};

static int32_t aclshmemi_init_attr_impl(
    aclshmemx_bootstrap_t bootstrap_flags, aclshmemx_init_attr_t* attributes, InitHeapMode heap_mode,
    const void* buffer_descs, size_t buffer_count)
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    SHM_ASSERT_RETURN(attributes != nullptr, ACLSHMEM_INVALID_PARAM);
    const bool use_user_buffer_heap = heap_mode == InitHeapMode::USER_BUFFER_HEAP;
    int32_t ret;
    uint64_t id = attributes->instance_id;

    // Multi-instance create ctx. Only install rollback for newly created
    // instances — re-init of an existing instance must not destroy it on failure.
    bool is_new_instance = (id != 0 && aclshmem_ctx_domain.find(id) == aclshmem_ctx_domain.end());
    ACLSHMEM_CHECK_RET(aclshmemi_instance_ctx_create(attributes));
    auto ctx_guard = shm::utils::make_scope_guard(static_cast<void*>(nullptr), [id, is_new_instance](void*) {
        if (is_new_instance) {
            (void)aclshmemi_instance_ctx_destroy(id);
        }
    });
    ACLSHMEM_CHECK_RET(aclshmemx_instance_ctx_set_impl(id));

    bool init_succeeded = false;
    bool bootstrap_initialized = false;
    bool backend_counted = false;
    bool entity_bound = false;
    bool device_state_initialized = false;
    bool heap_reserved = false;
    auto init_abort_guard = shm::utils::make_scope_guard(static_cast<void*>(nullptr), [&](void*) {
        if (init_succeeded) {
            return;
        }

        SHM_LOG_WARN("ACL SHMEM initialization failed; releasing partial resources without synchronization.");
        memory_manager_destroy();
        if (init_manager != nullptr && entity_bound) {
            if (heap_reserved) {
                (void)init_manager->remove_heap();
            }
            (void)init_manager->release_heap(HOST_SIDE);
            (void)init_manager->release_heap();
            if (device_state_initialized) {
                (void)init_manager->finalize_device_state();
            }
            (void)init_manager->release_aclshmem_entity(id);
        }
        if (bootstrap_initialized) {
            aclshmemi_bootstrap_finalize();
        }
        if (backend_counted) {
            --g_init_manager_count;
            if (g_init_manager_count == 0) {
                delete init_manager;
                init_manager = nullptr;
            }
        }
        g_state.is_aclshmem_initialized = false;
    });

    // config init
    ACLSHMEM_CHECK_RET(
        aclshmemx_init_status() != ACLSHMEM_STATUS_NOT_INITIALIZED,
        "SHMEM has been initialized, do not call init interface repeatedly!", ACLSHMEM_INNER_ERROR);
    ACLSHMEM_CHECK_RET(aclshmemx_set_log_level(aclshmem_log::ERROR_LEVEL));
    ACLSHMEM_CHECK_RET(
        use_user_buffer_heap ? check_user_buffer_heap_attr(attributes) : check_attr(attributes),
        "An error occurred while checking the initialization attributes. Please check the initialization parameters.");
    ACLSHMEM_CHECK_RET(version_compatible(), "ACLSHMEM Version mismatch.");

    // init bootstrap
    ACLSHMEM_CHECK_RET(aclshmemi_bootstrap_init(bootstrap_flags, attributes));
    bootstrap_initialized = true;

    uint64_t heap_size = attributes->local_mem_size + ACLSHMEM_EXTRA_SIZE;
    uint64_t external_bytes = 0;
    std::unique_ptr<shm::UserBufferHeapInput> user_buffer_heap_input;
    if (use_user_buffer_heap) {
#ifdef HAS_ACLRT_MEM_FABRIC_HANDLE
        const auto* buffers = static_cast<const aclshmemx_buffer_desc_t*>(buffer_descs);
        shm::UserBufferHeapLayoutHeader local_header{};
        local_header.local_status = shm::DlApi::LoadAclLibrary();
        if (local_header.local_status == ACLSHMEM_SUCCESS) {
            (void)build_local_layout_header(buffers, buffer_count, attributes, &user_buffer_heap_input, &local_header);
        }
        std::vector<shm::UserBufferHeapLayoutHeader> all_headers;
        try {
            all_headers.resize(static_cast<size_t>(attributes->n_pes));
        } catch (const std::bad_alloc&) {
            return ACLSHMEM_MALLOC_FAILED;
        }
        ACLSHMEM_CHECK_RET(g_boot_handle.allgather(
            &local_header, all_headers.data(), static_cast<int>(sizeof(local_header)), &g_boot_handle));
        ACLSHMEM_CHECK_RET(validate_layout_headers(all_headers, attributes->n_pes));
        SHM_ASSERT_RETURN(user_buffer_heap_input != nullptr, ACLSHMEM_INNER_ERROR);
        ACLSHMEM_CHECK_RET(validate_size_vectors(*user_buffer_heap_input, attributes->n_pes, &g_boot_handle));
        external_bytes = local_header.external_bytes;
        heap_size = local_header.heap_size;
#else
        return ACLSHMEM_NOT_SUPPORTED;
#endif
    }
    ACLSHMEM_CHECK_RET(aclshmemi_state_init_attr(attributes, heap_size));

    // init backend for memory manager
    g_init_manager_count++;
    backend_counted = true;
    if (init_manager == nullptr) {
        init_manager = new aclshmemi_init_backend();
    } else {
        SHM_LOG_INFO("init_manager already exists, skipping creation. ");
    }

    // aclshmem_entity init
    ACLSHMEM_CHECK_RET(init_manager->bind_aclshmem_entity(
        attributes, &g_state, &g_boot_handle, std::move(user_buffer_heap_input), g_udma_qp_config,
        g_rdma_qp_config.qpNum, g_sdma_qp_config.qpNum));
    entity_bound = true;
    ACLSHMEM_CHECK_RET(init_manager->init_device_state());
    device_state_initialized = true;
    auto exception_guard =
        shm::utils::make_scope_guard(static_cast<void*>(nullptr), [](void*) { aclshmemi_exception_report_finalize(); });
    ACLSHMEM_CHECK_RET(aclshmemi_exception_report_apply_deferred_config(attributes->option_attr.data_op_engine_type));
    int32_t reserve_status = init_manager->reserve_heap();
    if (use_user_buffer_heap) {
        ACLSHMEM_CHECK_RET(aclshmemi_collective_status_gate(reserve_status, attributes->n_pes, &g_boot_handle));
    } else {
        ACLSHMEM_CHECK_RET(reserve_status);
    }
    heap_reserved = true;
    ACLSHMEM_CHECK_RET(init_manager->setup_heap());

    // shmem submodules init
    auto* allocator_base = static_cast<uint8_t*>(g_state.heap_base) + external_bytes;
    ACLSHMEM_CHECK_RET(memory_manager_initialize(allocator_base, g_state.heap_size - external_bytes));

    if (check_support_d2h()) {
        // only reserve dramp heap, skip setup_heap for host dram, setup heap when malloc on host
        ACLSHMEM_CHECK_RET(init_manager->reserve_heap(HOST_SIDE));
    }
    uint32_t aiv_count = 0;
    const int32_t core_sync_config_status = query_vector_core_count(aiv_count);
    ACLSHMEM_CHECK_RET(aclshmemi_collective_status_gate(core_sync_config_status, g_state.npes, &g_boot_handle));
    ACLSHMEM_CHECK_RET(aclshmemi_signal_init());
    ACLSHMEM_CHECK_RET(aclshmemi_team_init(g_state.mype, g_state.npes, aiv_count));
    ACLSHMEM_CHECK_RET(aclshmemi_sync_init());
    ACLSHMEM_CHECK_RET(init_manager->build_topology_state(g_instance_ctx->id));
    g_state.is_aclshmem_initialized = true;
    auto current_context = aclshmem_resource_domain.find(g_instance_ctx->id);
    ACLSHMEM_CHECK_RET(
        current_context == aclshmem_resource_domain.end() || current_context->second == nullptr,
        "Current instance profiling context does not exist.", ACLSHMEM_INNER_ERROR);
    ACLSHMEM_CHECK_RET(prof_util_init(&current_context->second->host_profs, &g_state));
    ACLSHMEM_CHECK_RET(update_device_state());
    ACLSHMEM_CHECK_RET(aclshmemi_control_barrier_all());
    SHM_LOG_INFO("The ACLSHMEM pe: " << aclshmem_my_pe() << " init success.");
    exception_guard.release();
    init_succeeded = true;
    g_qp_config_frozen = true;
    init_abort_guard.release();
    ctx_guard.release();
    return ACLSHMEM_SUCCESS;
}

namespace {
constexpr uint32_t TOPOLOGY_V1_SIZE = 64U;
static_assert(sizeof(void*) == 8 && alignof(aclshmemx_topology_t) == 8);
static_assert(sizeof(aclshmemx_topology_t) == TOPOLOGY_V1_SIZE);
static_assert(offsetof(aclshmemx_topology_t, struct_size) == 0);
static_assert(offsetof(aclshmemx_topology_t, abi_version) == 4);
static_assert(offsetof(aclshmemx_topology_t, engine_mask) == 8);
static_assert(offsetof(aclshmemx_topology_t, net_layer_count) == 12);
static_assert(offsetof(aclshmemx_topology_t, net_layers) == 16);
static_assert(offsetof(aclshmemx_topology_t, reserved) == 24);
} // namespace

int aclshmemx_query_topology(int pe, uint32_t query_mask, aclshmemx_topology_t* topology)
{
    if (topology == nullptr) {
        return ACLSHMEM_INVALID_PARAM;
    }
    constexpr uint32_t defined_mask = ACLSHMEMX_TOPOLOGY_QUERY_ENGINES | ACLSHMEMX_TOPOLOGY_QUERY_LINKS;
    if (query_mask == 0 || (query_mask & ~defined_mask) != 0 ||
        topology->struct_size < TOPOLOGY_V1_SIZE || topology->abi_version != ACLSHMEMX_TOPOLOGY_ABI_VERSION) {
        return ACLSHMEM_INVALID_VALUE;
    }
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (init_manager == nullptr || g_instance_ctx == nullptr || !g_state.is_aclshmem_initialized) {
        return ACLSHMEM_NOT_INITED;
    }
    if (pe < 0 || pe >= g_state.npes) {
        return ACLSHMEM_INVALID_VALUE;
    }
    return init_manager->query_topology(g_instance_ctx->id, pe, query_mask, topology);
}

int32_t aclshmemx_init_attr(aclshmemx_bootstrap_t bootstrap_flags, aclshmemx_init_attr_t* attributes)
{
    return aclshmemi_init_attr_impl(bootstrap_flags, attributes, InitHeapMode::DEFAULT_HEAP, nullptr, 0);
}

int32_t aclshmemx_init_attr_with_buffers(
    aclshmemx_bootstrap_t bootstrap_flags, aclshmemx_init_attr_t* attributes, const aclshmemx_buffer_desc_t* buffers,
    size_t buffer_count)
{
#ifndef HAS_ACLRT_MEM_FABRIC_HANDLE
    (void)bootstrap_flags;
    (void)attributes;
    (void)buffers;
    (void)buffer_count;
    return ACLSHMEM_NOT_SUPPORTED;
#else
    SHM_ASSERT_RETURN(attributes != nullptr, ACLSHMEM_INVALID_PARAM);
    return aclshmemi_init_attr_impl(bootstrap_flags, attributes, InitHeapMode::USER_BUFFER_HEAP, buffers, buffer_count);
#endif
}

static int32_t aclshmemi_finalize_impl(uint64_t instance_id)
{
    // Only switch context when finalizing a non-active instance; finalizing the
    // currently active instance needs no switch (globals already hold its state).
    // set_impl also validates that instance_id exists, preventing accidental
    // release of the active instance when an unknown id is passed.
    if (instance_id != g_instance_ctx->id) {
        ACLSHMEM_CHECK_RET(aclshmemx_instance_ctx_set_impl(instance_id));
    }

    SHM_LOG_INFO("The instance : " << instance_id << ", The pe: " << aclshmem_my_pe() << " begins to finalize.");
    if (init_manager == nullptr) {
        SHM_LOG_INFO("init_manager is null finalize success.");
        g_state.is_aclshmem_initialized = false;
        ACLSHMEM_CHECK_RET(aclshmemi_instance_ctx_destroy(instance_id));
        return ACLSHMEM_SUCCESS;
    }

    if (g_init_manager_count == 1) {
        aclshmemi_exception_report_finalize();
    }

    ACLSHMEM_CHECK_RET(aclshmemi_control_barrier_all());

    // shmem submodules finalize
    ACLSHMEM_CHECK_RET(aclshmemi_team_finalize());
    ACLSHMEM_CHECK_RET(aclshmemi_signal_finalize());
    memory_manager_destroy();

    // From this point onward the runtime submodules are no longer usable.  Keep
    // is_aclshmem_created set until every destructive heap-cleanup step has
    // completed, so a cleanup failure is reported as SHM_CREATED rather than as
    // a fully initialized (or fully finalized) instance.
    g_state.is_aclshmem_initialized = false;

    int32_t firstCleanupStatus = ACLSHMEM_SUCCESS;
    auto recordCleanupStatus = [&](int32_t status, const char* operation) {
        if (status == ACLSHMEM_SUCCESS) {
            return;
        }
        SHM_LOG_ERROR(operation << " failed during finalize, ret=" << status);
        if (firstCleanupStatus == ACLSHMEM_SUCCESS) {
            firstCleanupStatus = status;
        }
    };

    // shmem basic finalize
    const int32_t removeStatus = init_manager->remove_heap();
    recordCleanupStatus(removeStatus, "remove_heap");
    recordCleanupStatus(
        aclshmemi_collective_status_gate(removeStatus, g_state.npes, &g_boot_handle),
        "collective remove_heap status gate");
    recordCleanupStatus(init_manager->release_heap(), "release_heap");
    recordCleanupStatus(init_manager->finalize_device_state(), "finalize_device_state");
    if (check_support_d2h()) {
        recordCleanupStatus(init_manager->remove_heap(HOST_SIDE), "remove host heap");
        recordCleanupStatus(init_manager->release_heap(HOST_SIDE), "release host heap");
    }
    recordCleanupStatus(init_manager->release_aclshmem_entity(instance_id), "release ACLSHMEM entity");
    if (g_state_host.default_stream != nullptr) {
        auto ret = aclrtSynchronizeStream(g_state_host.default_stream);
        recordCleanupStatus(ret, "synchronize default stream");
        ret = aclrtDestroyStream(g_state_host.default_stream);
        recordCleanupStatus(ret, "destroy default stream");
        g_state_host.default_stream = nullptr;
    }

    // Synchronize all ranks before destroying the store, so that no rank
    // can start the next init while another rank's store is still alive.
    recordCleanupStatus(aclshmemi_control_barrier_all(), "finalize control barrier");
    aclshmemi_bootstrap_finalize();

    // Only when the process has no instance, release init_manager.
    g_init_manager_count--;
    const bool is_last_instance = (g_init_manager_count == 0);
    if (is_last_instance) {
        delete init_manager;
        init_manager = nullptr;
    }
    if (firstCleanupStatus == ACLSHMEM_SUCCESS) {
        SHM_LOG_INFO("The pe: " << aclshmem_my_pe() << " finalize success.");
    } else {
        SHM_LOG_ERROR(
            "The pe: " << aclshmem_my_pe() << " completed finalize cleanup with error: " << firstCleanupStatus);
    }
    g_state.is_aclshmem_initialized = false;
    recordCleanupStatus(aclshmemi_instance_ctx_destroy(instance_id), "destroy instance context");
    if (is_last_instance) {
        g_rdma_qp_config = shm::transport::TransportOptions::RdmaQpConfig{};
        g_udma_qp_config = shm::transport::UdmaQpConfig{};
        g_sdma_qp_config = shm::transport::SdmaQpConfig{};
        g_qp_config_frozen = false;
    }
    return firstCleanupStatus;
}

int32_t aclshmemx_finalize(uint64_t instance_id)
{
    SHM_LOG_INFO("aclshmemx_finalize called with instance_id=" << instance_id);
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    return aclshmemi_finalize_impl(instance_id);
}

int32_t aclshmem_finalize(void)
{
    SHM_LOG_INFO("aclshmem_finalize called.");
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    return aclshmemi_finalize_impl(g_instance_ctx->id);
}

void aclshmem_info_get_version(int* major, int* minor)
{
    SHM_ASSERT_RET_VOID(major != nullptr && minor != nullptr);
    *major = ACLSHMEM_MAJOR_VERSION;
    *minor = ACLSHMEM_MINOR_VERSION;
}

void aclshmem_info_get_name(char* name)
{
    SHM_ASSERT_RET_VOID(name != nullptr);
    std::ostringstream oss;
    oss << "ACLSHMEM v" << ACLSHMEM_VENDOR_MAJOR_VER << "." << ACLSHMEM_VENDOR_MINOR_VER << "."
        << ACLSHMEM_VENDOR_PATCH_VER;
    auto version_str = oss.str();
    size_t i;
    for (i = 0; i < ACLSHMEM_MAX_NAME_LEN - 1 && version_str[i] != '\0'; i++) {
        name[i] = version_str[i];
    }
    name[i] = '\0';
}

int32_t aclshmemx_get_uniqueid(aclshmemx_uniqueid_t* uid)
{
    int status = aclshmemx_set_log_level(aclshmem_log::ERROR_LEVEL);

    ACLSHMEM_CHECK_RET(
        aclshmemi_bootstrap_pre_init(ACLSHMEMX_INIT_WITH_UNIQUEID, &g_boot_handle),
        "Get uniqueid failed during the bootstrap preloading step.");

    aclshmemx_uniqueid_t default_uid{};
    default_uid.version = ACLSHMEM_UNIQUEID_VERSION;
    *uid = default_uid;
    if (g_boot_handle.pre_init_ops) {
        ACLSHMEM_CHECK_RET(
            g_boot_handle.pre_init_ops->get_unique_id((void*)uid), "Get uniqueid failed during the get uniqueid step.");
    } else {
        SHM_LOG_ERROR("Pre_init_ops is empty, unique_id cannot be obtained.");
        status = ACLSHMEM_INVALID_PARAM;
    }
    return status;
}

int32_t aclshmemx_set_log_level(int level)
{
    // use env first, input level secondly, user may change level from env instead call func
    const char* in_level = std::getenv("SHMEM_LOG_LEVEL");
    if (in_level != nullptr) {
        auto tmp_level = std::string(in_level);
        if (tmp_level == "DEBUG") {
            level = aclshmem_log::DEBUG_LEVEL;
        } else if (tmp_level == "INFO") {
            level = aclshmem_log::INFO_LEVEL;
        } else if (tmp_level == "WARN") {
            level = aclshmem_log::WARN_LEVEL;
        } else if (tmp_level == "ERROR") {
            level = aclshmem_log::ERROR_LEVEL;
        } else if (tmp_level == "FATAL") {
            level = aclshmem_log::FATAL_LEVEL;
        }
    }

    return aclshmem_log::aclshmem_out_logger::Instance().set_log_level(static_cast<aclshmem_log::log_level>(level));
}

int32_t aclshmemx_set_conf_store_tls(bool enable, const char* tls_info, const uint32_t tls_info_len)
{
    g_boot_handle.tls_enable = enable;
    g_boot_handle.tls_info = tls_info;
    g_boot_handle.tls_info_len = tls_info_len;

    return ACLSHMEM_SUCCESS;
}

void aclshmem_rank_exit(int status)
{
    SHM_LOG_DEBUG("aclshmem_rank_exit is work ,status: " << status);
    exit(status);
}

int32_t aclshmemx_set_config_store_tls_key(
    const char* tls_pk, const uint32_t tls_pk_len, const char* tls_pk_pw, const uint32_t tls_pk_pw_len,
    const aclshmem_decrypt_handler decrypt_handler)
{
    g_boot_handle.tls_pk = tls_pk;
    g_boot_handle.tls_pk_len = tls_pk_len;
    g_boot_handle.tls_pk_pw = tls_pk_pw;
    g_boot_handle.tls_pk_pw_len = tls_pk_pw_len;
    g_boot_handle.decrypt_handler = decrypt_handler;

    return ACLSHMEM_SUCCESS;
}

int32_t aclshmemx_set_extern_logger(void (*func)(int level, const char* msg))
{
    aclshmem_log::aclshmem_out_logger::Instance().set_extern_log_func(func);
    return ACLSHMEM_SUCCESS;
}

void aclshmem_global_exit(int status)
{
    if (g_boot_handle.is_bootstraped == true) {
        g_boot_handle.global_exit(status, &g_boot_handle);
    }
    SHM_LOG_WARN("Bootstrap not initialized. Global_exit Do nothing. ");
}

void aclshmemx_show_prof()
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    auto current_context = aclshmem_resource_domain.find(g_instance_ctx->id);
    if (current_context == aclshmem_resource_domain.end()) {
        SHM_LOG_ERROR("Current instance profiling context does not exist.");
        return;
    }
    prof_data_print(&current_context->second->host_profs, &g_state, nullptr, true);
}

void aclshmemx_get_prof(aclshmem_prof_pe_t** out_profs, bool verbose)
{
    std::lock_guard<std::mutex> lock(g_aclshmem_ctx_mutex);
    if (out_profs != nullptr) {
        *out_profs = nullptr;
    }
    // 如果 verbose 为 false 且 out_profs 为 nullptr，直接返回
    if (!verbose && out_profs == nullptr) {
        return;
    }
    auto current_context = aclshmem_resource_domain.find(g_instance_ctx->id);
    if (current_context == aclshmem_resource_domain.end()) {
        SHM_LOG_ERROR("Current instance profiling context does not exist.");
        return;
    }
    prof_data_print(&current_context->second->host_profs, &g_state, out_profs, verbose);
}
