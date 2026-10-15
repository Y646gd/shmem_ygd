/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#ifndef SHMEM_EXAMPLE_TOPOLOGY_PLAN_H
#define SHMEM_EXAMPLE_TOPOLOGY_PLAN_H
#include <iostream>
#include "shmem.h"

// This demo uses QP 0 for layer 0 and QP 1 for layer 1. Verify the actual
// resources after initialization: a topology relation alone cannot choose a QP.
inline int VerifyTwoLayerPlan(int peer)
{
    aclshmemx_topology_t info = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    int status = aclshmemx_query_topology(peer,
        ACLSHMEMX_TOPOLOGY_QUERY_ENGINES | ACLSHMEMX_TOPOLOGY_QUERY_LINKS, &info);
    if (status != ACLSHMEM_SUCCESS) return status;
    if (!(info.engine_mask & ACLSHMEM_DATA_OP_UDMA)) return ACLSHMEM_NOT_SUPPORTED;
    bool found[2] = {false, false};
    for (uint32_t i = 0; i < info.net_layer_count; ++i) {
        const auto layer = static_cast<uint32_t>(info.net_layers[i]);
        if (layer < 2) found[layer] = true;
    }
    if (!found[0] || !found[1]) return ACLSHMEM_NOT_SUPPORTED;
    uint32_t count = 0;
    status = aclshmemx_get_qp_num(ACLSHMEM_DATA_OP_UDMA, &count);
    if (status != ACLSHMEM_SUCCESS) return status;
    if (count != 2) return ACLSHMEM_INVALID_VALUE;
    for (uint32_t qp = 0; qp < 2; ++qp) {
        aclshmemx_net_layer_t layer = ACLSHMEM_NET_LAYER_DEFAULT;
        status = aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, peer, qp, &layer);
        if (status != ACLSHMEM_SUCCESS) return status;
        if (static_cast<uint32_t>(layer) != qp) return ACLSHMEM_NOT_SUPPORTED;
    }
    std::cout << "[TOPOLOGY] peer=" << peer << " engines=" << info.engine_mask
              << " layers include 0,1; verified actual QP 0 -> layer 0, QP 1 -> layer 1\n";
    return ACLSHMEM_SUCCESS;
}
#endif
