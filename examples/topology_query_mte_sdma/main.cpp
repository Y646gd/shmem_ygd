/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#include "engine_query_common.h"

int main(int argc, char** argv)
{
    // Enable MTE + SDMA before initialization; verify both bits on every remote PE.
    constexpr auto engines = static_cast<data_op_engine_type_t>(ACLSHMEM_DATA_OP_MTE | ACLSHMEM_DATA_OP_SDMA);
    return shmem_examples::RunEngineQuery(argc, argv, engines);
}
