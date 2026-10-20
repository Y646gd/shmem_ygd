/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#include <iostream>
#include <stdexcept>
#include <string>
#include "acl/acl.h"
#include "shmem.h"
#include "utils.h"

// Matrix mode prints failed queries without consuming their output fields.
static void print_topology(int peer, uint32_t mask)
{
    aclshmemx_topology_t info = ACLSHMEMX_TOPOLOGY_INITIALIZER;
    const int status = aclshmemx_query_topology(peer, mask, &info);
    std::cout << "pe=" << peer << " query=0x" << std::hex << mask << std::dec << " status=" << status;
    if (status == ACLSHMEM_SUCCESS) {
        std::cout << " engine_mask=0x" << std::hex << info.engine_mask << std::dec
                  << " net_layer_count=" << info.net_layer_count << " net_layers=[";
        for (uint32_t i = 0; i < info.net_layer_count; ++i)
            std::cout << (i ? "," : "") << static_cast<uint32_t>(info.net_layers[i]);
        std::cout << "]";
    }
    std::cout << '\n';
}

int main(int argc, char** argv)
{
    if (argc < 5 || argc > 7) {
        std::cerr << "Usage: topology_query PE NPES DEVICE tcp://HOST:PORT [links=0|1|2(matrix)] [engine_mask=1]\n";
        return 1;
    }
    int pe, npes, device, links, engines;
    try {
        auto number = [](const char* text) {
            size_t used = 0;
            const int value = std::stoi(text, &used);
            if (text[used] != '\0') throw std::invalid_argument("trailing characters");
            return value;
        };
        pe = number(argv[1]);
        npes = number(argv[2]);
        device = number(argv[3]);
        links = argc >= 6 ? number(argv[5]) : 0;
        engines = argc >= 7 ? number(argv[6]) : ACLSHMEM_DATA_OP_MTE;
        if (pe < 0 || pe >= npes || npes > ACLSHMEM_MAX_PES || device < 0 ||
            (links < 0 || links > 2) || engines < 0 || (engines & ~15) != 0) {
            throw std::invalid_argument("argument out of range");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    int status = aclInit(nullptr);
    if (status != 0) return status;
    status = aclrtSetDevice(device);
    if (status != 0) { aclFinalize(); return status; }
    if (links == 2) {
        std::cout << "case=not_initialized ";
        print_topology(0, 1);
    }
    aclshmemx_uniqueid_t uid{};
    aclshmemx_init_attr_t attr{};
    status = test_set_attr(pe, npes, 64ULL * 1024 * 1024, argv[4], uid, &attr);
    attr.option_attr.data_op_engine_type = static_cast<data_op_engine_type_t>(engines);
    if (status == 0) status = aclshmemx_init_attr(ACLSHMEMX_INIT_WITH_DEFAULT, &attr);
    std::cout << "config=0x" << std::hex << engines << std::dec << " local_pe=" << pe
              << " init_status=" << status << '\n';
    if (status == 0) {
        if (links == 2) {
            for (int peer = 0; peer < npes; ++peer)
                for (uint32_t mask : {1U, 2U, 3U, 0U, 4U}) print_topology(peer, mask);
            print_topology(-1, 1);
            print_topology(npes, 1);
            std::cout << "case=null_result status=" << aclshmemx_query_topology(0, 1, nullptr) << '\n';
            aclshmemx_topology_t invalid = ACLSHMEMX_TOPOLOGY_INITIALIZER;
            invalid.struct_size = 63;
            std::cout << "case=short_result status=" << aclshmemx_query_topology(0, 1, &invalid) << '\n';
            invalid.struct_size = sizeof(invalid);
            invalid.abi_version = ACLSHMEMX_TOPOLOGY_ABI_VERSION + 1;
            std::cout << "case=abi_mismatch status=" << aclshmemx_query_topology(0, 1, &invalid) << '\n';
        }
        // Keep the original single-query mode and output intact.
        const uint32_t mask = ACLSHMEMX_TOPOLOGY_QUERY_ENGINES | (links ? ACLSHMEMX_TOPOLOGY_QUERY_LINKS : 0U);
        for (int peer = 0; links != 2 && peer < npes; ++peer) {
            aclshmemx_topology_t info = ACLSHMEMX_TOPOLOGY_INITIALIZER;
            const int ret = aclshmemx_query_topology(peer, mask, &info);
            if (ret != 0) {
                std::cerr << "query failed: " << ret << '\n';
                status = ret;
                break;
            }
            std::cout << "PE " << pe << " -> " << peer << " engines=" << info.engine_mask << " layers=[";
            for (uint32_t i = 0; i < info.net_layer_count; ++i) {
                std::cout << (i ? "," : "") << static_cast<uint32_t>(info.net_layers[i]);
            }
            std::cout << "]\n";
        }
        const int finalized = aclshmem_finalize();
        if (status == 0) status = finalized;
    }
    aclrtResetDevice(device);
    aclFinalize();
    return status;
}
