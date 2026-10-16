/**
 * @cond IGNORE_COPYRIGHT
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 * @endcond
 */
#ifndef SHMEM_HOST_INIT_H
#define SHMEM_HOST_INIT_H

#include "host/shmem_host_def.h"
#include "host_device/shmem_common_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque device physical-allocation handle accepted by the user-buffer initialization API.
 *
 * The concrete runtime handle type is intentionally hidden from the public ACLSHMEM ABI. Pass the handle value
 * returned by the platform runtime without transferring ownership.
 */
typedef void* aclshmemx_drv_mem_handle_t;

/**
 * @brief Opaque pointer to a fabric share handle accepted by the user-buffer initialization API.
 *
 * The pointer addresses a platform-provided share-handle value. ACLSHMEM copies the value during initialization and
 * does not retain the pointer.
 */
typedef const void* aclshmemx_mem_fabric_handle_t;

/**
 * @struct aclshmemx_buffer_optional_attr_t
 * @brief Optional sharing attributes for one caller-provided device buffer.
 *
 * @details
 * - `mem_handle` is optional. The caller retains ownership of a non-NULL handle and must keep it valid until
 *   ACLSHMEM finalization completes. A non-NULL handle must identify the single physical allocation that backs the
 *   descriptor's complete `[addr, addr + size)` range. When NULL, ACLSHMEM obtains and owns the references it needs.
 * - `fabric_handle` is optional. The pointed-to value must remain valid until
 *   aclshmemx_init_attr_with_buffers() returns; ACLSHMEM copies the value and does not take ownership. A non-NULL
 *   fabric handle must describe the same complete physical allocation as `addr` and `mem_handle`, when both handles
 *   are supplied. ACLSHMEM does not validate handle-to-address association; the caller must satisfy this precondition.
 * - `reserved` is reserved for future ABI-compatible extensions. The caller must initialize every element to zero.
 */
typedef struct aclshmemx_buffer_optional_attr_t {
    aclshmemx_drv_mem_handle_t mem_handle = nullptr;
    aclshmemx_mem_fabric_handle_t fabric_handle = nullptr;
    uint64_t reserved[6] = {};
} aclshmemx_buffer_optional_attr_t;

/**
 * @struct aclshmemx_buffer_desc_t
 * @brief Describes one caller-mapped device buffer used as a fixed segment of the symmetric heap.
 *
 * @details
 * - `addr` is required and identifies the base of an existing local device-buffer mapping. The mapping must remain
 *   valid until the collective initialization call returns. Descriptor order defines the fixed symmetric prefix.
 * - `size` is required, is measured in bytes, must be nonzero, and must be aligned to the allocation's minimum
 *   granularity.
 * - `optional_attr` may be NULL. A non-NULL structure must remain valid until
 *   aclshmemx_init_attr_with_buffers() returns; ACLSHMEM does not take ownership.
 * - `reserved` is reserved for future ABI-compatible extensions. The caller must initialize every element to zero.
 */
typedef struct aclshmemx_buffer_desc_t {
    void* addr = nullptr;
    uint64_t size = 0;
    const aclshmemx_buffer_optional_attr_t* optional_attr = nullptr;
    uint64_t reserved[5] = {};
} aclshmemx_buffer_desc_t;

static_assert(sizeof(aclshmemx_buffer_optional_attr_t) == 64, "aclshmemx_buffer_optional_attr_t must be 64 bytes.");
static_assert(
    alignof(aclshmemx_buffer_optional_attr_t) == alignof(uint64_t),
    "aclshmemx_buffer_optional_attr_t must be 8-byte aligned.");
static_assert(sizeof(aclshmemx_buffer_desc_t) == 64, "aclshmemx_buffer_desc_t must be 64 bytes.");
static_assert(alignof(aclshmemx_buffer_desc_t) == alignof(uint64_t), "aclshmemx_buffer_desc_t must be 8-byte aligned.");

/**
 * @brief Query the current initialization status.
 *
 * @return Returns initialization status. Returning ACLSHMEM_STATUS_IS_INITIALIZED indicates that initialization is
 *         complete. All return types can be found in <b>\ref aclshmemx_init_status_t</b>.
 */
ACLSHMEM_HOST_API int aclshmemx_init_status(void);
#define shmem_init_status aclshmemx_init_status

/**
 * @brief get the unique id and return it by input argument uid. This function need run with PTA.
 *
 * @param uid               [out] a ptr to uid generate by shmem
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmemx_get_uniqueid(aclshmemx_uniqueid_t* uid);
#define shmem_get_uniqueid aclshmemx_get_uniqueid

/**
 * @brief init process with unique id. This function need run with PTA.
 *
 * @param my_pe                 [in] Local PE ID, must be less than the maximum supported PEs (my_pe < ACLSHMEM_MAX_PES)
 * @param n_pes                 [in] Total number of PEs, must be less than or equal to the maximum supported PEs (n_pes
 * <= ACLSHMEM_MAX_PES)
 * @param local_mem_size        [in] Allocated local memory size , must be less than the maximum supported local memory
 * size (local_mem_size < ACLSHMEM_MAX_LOCAL_SIZE)
 * @param uid                   [in] Unique ID obtained from <b>aclshmemx_get_uniqueid()</b>
 * @param aclshmem_attr         [in/out] Attribute struct, output parameter of this interface and input parameter for
 * subsequent initialization functions <b>aclshmemx_init_attr()</b>
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmemx_set_attr_uniqueid_args(
    int my_pe, int n_pes, int64_t local_mem_size, aclshmemx_uniqueid_t* uid, aclshmemx_init_attr_t* aclshmem_attr);
#define shmem_set_attr_uniqueid_args aclshmemx_set_attr_uniqueid_args

/**
 * @brief Configure the process-wide number of QPs created for a data operation engine.
 *
 * @note Valid ranges and resource scopes differ per engine:
 *       - SDMA: [1, min(device vector-core count, ACLSHMEM_MAX_AIV_PER_NPU)]. SDMA creates local device-only
 *         streams, one per QP; the streams are not bound to a specific peer. SDMA defaults to one QP.
 *       - UDMA: [1, ACLSHMEM_MAX_QP_NUM], one QP group per peer connection; supported on Ascend950.
 *       - ROCE: [1, ACLSHMEM_MAX_QP_NUM], one QP group per peer connection; requires the Ascend950 XSCALE or
 *         HNS_1825 backend.
 *       Call this interface when no ACLSHMEM instance is initialized. The configuration remains
 *       frozen while any instance is alive. After the last instance is finalized, the QP counts of every
 *       engine (SDMA, UDMA and ROCE) are reset to 1 and can be configured again before the next
 *       initialization. The value must be identical on every PE; inconsistent values produce incompatible
 *       metadata layouts. This function is thread-safe within a process and is serialized with ACLSHMEM
 *       initialization and finalization.
 *
 * @param engine              [in] Data operation engine: SDMA, UDMA, or ROCE.
 * @param qp_num              [in] Number of QPs; see the per-engine ranges above.
 * @return ACLSHMEM_SUCCESS on success, otherwise an ACLSHMEM error code.
 */
ACLSHMEM_HOST_API int aclshmemx_set_qp_num(data_op_engine_type_t engine, uint32_t qp_num);

typedef enum : uint32_t {
    ACLSHMEM_NET_LAYER_0 = 0U,                /**< Supported by aclshmemx_set_qp_net_layer; typically direct MESH. */
    ACLSHMEM_NET_LAYER_1 = 1U,                /**< Supported by aclshmemx_set_qp_net_layer; typically switched CLOS. */
    ACLSHMEM_NET_LAYER_2 = 2U,                /**< Reserved; not supported by aclshmemx_set_qp_net_layer. */
    ACLSHMEM_NET_LAYER_3 = 3U,                /**< RoCE topology layer; not supported by aclshmemx_set_qp_net_layer. */
    ACLSHMEM_NET_LAYER_DEFAULT = 0xFFFFFFFFU, /**< Clear the explicit policy and restore default route selection. */
} aclshmemx_net_layer_t;

#define ACLSHMEMX_TOPOLOGY_ABI_VERSION 1U
#define ACLSHMEMX_TOPOLOGY_QUERY_ENGINES 0x1U
#define ACLSHMEMX_TOPOLOGY_QUERY_LINKS 0x2U

/** Host topology result. Initialize with ACLSHMEMX_TOPOLOGY_INITIALIZER. */
typedef struct aclshmemx_topology_t {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t engine_mask;
    uint32_t net_layer_count;
    const aclshmemx_net_layer_t* net_layers;
    uint64_t reserved[5];
} aclshmemx_topology_t;

#define ACLSHMEMX_TOPOLOGY_INITIALIZER \
    {sizeof(aclshmemx_topology_t), ACLSHMEMX_TOPOLOGY_ABI_VERSION, 0U, 0U, nullptr, {0U}}

/**
 * @brief Locally query the current instance's connectivity to a world PE.
 * @param pe Target PE in [0, npes). The source is the current instance's my_pe.
 * @param query_mask Nonzero OR of QUERY_ENGINES and QUERY_LINKS.
 * @param topology Caller-owned result initialized with ACLSHMEMX_TOPOLOGY_INITIALIZER.
 * @note ENGINES returns data_op_engine_type_t bits from initialized reachability.
 *       LINKS is supported on Ascend950 only: sorted, unique layers 0/1/3, empty for self.
 *       It describes the initialization-time physical topology, independent of engine
 *       enablement, QP count/policy and live port health; it does not promise usable QPs.
 *       net_layers is borrowed, read-only instance storage, valid until that instance's
 *       finalize begins. Serialize finalize with use/copy of this array. Selecting another
 *       instance does not transfer ownership. Query is noncollective and O(1).
 *       The v1 Host layout is 64 bytes, aligned to 8. Only engine_mask, net_layer_count
 *       and net_layers are written; input header, reserved slots and any tail are preserved.
 * @return SUCCESS; INVALID_PARAM for null output; INVALID_VALUE for invalid mask, PE,
 *         ABI version or size; NOT_INITED before init; NOT_SUPPORTED for A2/A3 LINKS.
 *         On error consume only the status.
 */
ACLSHMEM_HOST_API int aclshmemx_query_topology(int pe, uint32_t query_mask, aclshmemx_topology_t* topology);


/**
 * @brief Configure the process-wide topology netLayer policy for one QP.
 *
 * @note Call before ACLSHMEM initialization. The policy applies to peers in the same layer-0 network instance.
 *       Repeated calls for the same QP replace the previous value. Pass ACLSHMEM_NET_LAYER_DEFAULT to remove an
 *
 * explicit entry and restore default route selection for that QP. The first implementation supports UDMA
 * netLayer 0
 * and 1 when multi-link is enabled.
 *       Before initialization, all participating PEs must use the same effective
 * configuration for this engine: multi-link mode, QP count, explicitly configured qp_idx set, and requested netLayer
 * for each configured QP. The policy is sparse; QPs without an explicit entry use default route selection. For
 * same-domain peers,
 *       an explicit entry selects the requested layer. Cross-domain peers ignore explicit layer
 * policies and use
 *       reachable CLOS candidates only because MESH routes are limited to the same layer-0 network
 * instance. Thus,
 *       the same QP is not required to resolve to the same actual netLayer for every peer.
 * The
 * policy is frozen while any ACLSHMEM instance is alive and remains available until the last instance is finalized.
 * Finalizing the last instance clears all explicit QP layer entries; configure the desired entries again before the
 * next initialization cycle.
 *
 * @code
 * // Execute the same effective configuration on every participating PE before initialization.
 * aclshmemx_set_qp_num(ACLSHMEM_DATA_OP_UDMA, 3);
 * aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 0, ACLSHMEM_NET_LAYER_0);
 *
 * aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 2, ACLSHMEM_NET_LAYER_1);
 * // Before initialization, an explicit
 * entry can be removed if this QP should use default route selection.
 *
 * aclshmemx_set_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, 2, ACLSHMEM_NET_LAYER_DEFAULT);
 * // QP 1 has no explicit entry
 * and therefore uses default route selection.
 * aclshmemx_init_attr(...);
 * @endcode
 *
 * @param engine              [in] Data operation engine; currently only UDMA is supported.
 * @param qp_idx              [in] QP index in [0, configured QP count).
 * @param net_layer           [in] Effective network layer requested for this QP. Product topology interpretation
 *                              may map a raw topology-file layer to this public layer value. Use layer 0 for a
 *
 * same-domain direct MESH route and layer 1 for a switched CLOS route; layers 2 and 3
 * are reserved and are not
 * currently supported by this setter. Use
 *                              ACLSHMEM_NET_LAYER_DEFAULT to clear this QP's
 * explicit policy.
 * @return ACLSHMEM_SUCCESS on success, otherwise an ACLSHMEM error code.
 */
ACLSHMEM_HOST_API int aclshmemx_set_qp_net_layer(
    data_op_engine_type_t engine, uint32_t qp_idx, aclshmemx_net_layer_t net_layer);

/**
 * @brief Query the effective network layer used by one established peer/QP route.
 *
 * @note Call this interface after ACLSHMEM initialization has completed.
 *
 * @param engine              [in] Data operation engine; currently only UDMA is supported.
 * @param peer                [in] Remote PE in the current instance.
 * @param qp_idx              [in] UDMA QP index for the remote PE.
 * @param net_layer           [out] Effective network layer of the route selected during initialization.
 * @return ACLSHMEM_SUCCESS on success, otherwise an ACLSHMEM error code.
 */
ACLSHMEM_HOST_API int aclshmemx_get_qp_net_layer(
    data_op_engine_type_t engine, int peer, uint32_t qp_idx, aclshmemx_net_layer_t* net_layer);

/**
 * @brief Query the process-wide QP count configured for a data operation engine.
 *
 * @param engine              [in] Data operation engine.
 * @param qp_num              [out] Configured QP count.
 * @return ACLSHMEM_SUCCESS on success, otherwise an ACLSHMEM error code.
 */
ACLSHMEM_HOST_API int aclshmemx_get_qp_num(data_op_engine_type_t engine, uint32_t* qp_num);

/**
 * @brief Initialize the resources required for ACLSHMEM task based on attributes.
 *        Attributes can be created by users or obtained by calling <b>aclshmemx_set_attr_uniqueid_args()</b>.
 *        if the self-created attr structure is incorrect, the initialization will fail.
 *        It is recommended to build the attributes by <b>aclshmemx_set_attr_uniqueid_args()</b>.
 *
 * @param bootstrap_flags   [in] bootstrap_flags for init.
 * @param attributes        [in] Pointer to the user-defined attributes.
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmemx_init_attr(aclshmemx_bootstrap_t bootstrap_flags, aclshmemx_init_attr_t* attributes);
#define shmem_init_attr aclshmemx_init_attr

/**
 * @brief Initialize ACLSHMEM with caller-mapped device buffers as the fixed prefix of the symmetric heap.
 *
 * The descriptors form a fixed symmetric-heap prefix in array order. All PEs must provide the same buffer_count,
 * the same size at each descriptor index, and the same attributes->local_mem_size. The local_mem_size bytes form
 * the allocatable capacity after that fixed prefix and do not include any descriptor's size.
 *
 * @param bootstrap_flags [in] Bootstrap mode used for initialization.
 * @param attributes      [in] Initialization attributes; local_mem_size may be zero for this interface.
 * @param buffers         [in] Non-NULL array of caller-provided device-buffer descriptors. The array and referenced
 *                             optional attributes must remain valid until this function returns.
 * @param buffer_count    [in] Number of descriptors in buffers; must be greater than zero.
 * @retval ACLSHMEM_SUCCESS Initialization completed successfully on every PE.
 * @retval ACLSHMEM_INVALID_PARAM A required pointer, descriptor field, alignment, reserved field, or cross-PE
 *                                layout is invalid or inconsistent.
 * @retval ACLSHMEM_INVALID_VALUE A size, count, or derived heap range is outside the supported range.
 * @retval ACLSHMEM_NOT_SUPPORTED The requested buffer attributes or initialization configuration is unsupported.
 * @return Other ACLSHMEM error codes may report bootstrap, allocation, mapping, or initialization failures.
 * @note Execution domain: Host only. This blocking collective returns after all PEs publish a complete symmetric
 *       heap and all initialization control operations finish.
 * @note Thread safety: Initialization and finalization are serialized by ACLSHMEM. Do not modify descriptors or
 *       their optional attributes while this function is executing.
 */
ACLSHMEM_HOST_API int aclshmemx_init_attr_with_buffers(
    aclshmemx_bootstrap_t bootstrap_flags, aclshmemx_init_attr_t* attributes, const aclshmemx_buffer_desc_t* buffers,
    size_t buffer_count);

/**
 * @brief Release all resources used by the CURRENT instance of the ACLSHMEM library.
 *
 *        Single-instance mode: finalizes instance 0, behaving identically to the
 *        legacy no-argument API.
 *
 *        Multi-instance mode: finalizes the instance currently active, i.e. the one
 *        most recently selected via aclshmemx_instance_ctx_set (or instance 0 if no
 *        instance has been set). To release a specific instance with this API, call
 *        aclshmemx_instance_ctx_set(instance_id) first, then call this function.
 *        After finalization, if the destroyed instance was non-zero, the context
 *        falls back to instance 0.
 *
 *        For multi-instance scenarios it is recommended to use aclshmemx_finalize
 *        directly, which releases a specific instance by id without an explicit
 *        context switch.
 *
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmem_finalize(void);
#define shmem_finalize aclshmem_finalize

/**
 * @brief Release resources of a SPECIFIC instance (multi-instance extension).
 *        Switches to the target instance context first (if it is not already
 *        active), then finalizes it. After finalization, if the destroyed
 *        instance was non-zero, the context falls back to instance 0.
 *        Recommended for multi-instance scenarios.
 *
 * @param instance_id the unique identifier of the instance to finalize.
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmemx_finalize(uint64_t instance_id);
#define shmemx_finalize aclshmemx_finalize

/**
 * @brief returns the major and minor version.
 *
 * @param major [out] major version
 *
 * @param minor [out] minor version
 */
ACLSHMEM_HOST_API void aclshmem_info_get_version(int* major, int* minor);
#define shmem_info_get_version aclshmem_info_get_version

/**
 * @brief returns the vendor defined name string.
 *
 * @param name [out] name
 */
ACLSHMEM_HOST_API void aclshmem_info_get_name(char* name);
#define shmem_info_get_name aclshmem_info_get_name

/**
 * @brief Set the TLS private key and password, and register a decrypt key password handler.
 *
 * @param tls_pk the content of tls private key
 * @param tls_pk_len length of tls private key
 * @param tls_pk_pw the content of tls private key password
 * @param tls_pk_pw_len length of tls private key password
 * @param decrypt_handler decrypt function pointer
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int32_t aclshmemx_set_config_store_tls_key(
    const char* tls_pk, const uint32_t tls_pk_len, const char* tls_pk_pw, const uint32_t tls_pk_pw_len,
    const aclshmem_decrypt_handler decrypt_handler);
#define shmem_set_config_store_tls_key aclshmemx_set_config_store_tls_key

/**
 * @brief exit all ranks.
 *
 * @param status [in] exit status code
 */
ACLSHMEM_HOST_API void aclshmem_global_exit(int status);
#define shmem_global_exit aclshmem_global_exit

/**
 * @brief aclshmemx_set_conf_store_tls.
 *
 * @param enable whether to enable tls
 * @param tls_info the format described in memfabric SECURITYNOTE.md, if disabled tls_info won't be use
 * @param tls_info_len length of tls_info, if disabled tls_info_len won't be use
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int32_t aclshmemx_set_conf_store_tls(bool enable, const char* tls_info, const uint32_t tls_info_len);
#define shmem_set_conf_store_tls aclshmemx_set_conf_store_tls

/**
 * @brief Get the current instance context.
 *
 * @return Returns a pointer to the current instance context on success or NULL on failure.
 */
ACLSHMEM_HOST_API aclshmem_instance_ctx* aclshmemx_instance_ctx_get();

/**
 * @brief Set the current instance context by instance_id.
 *
 * @param instance_id the unique identifier of the instance to set as the current context.
 * @return Returns 0 on success or an error code on failure
 */
ACLSHMEM_HOST_API int aclshmemx_instance_ctx_set(uint64_t instance_id);

#ifdef __cplusplus
}
#endif

#endif
