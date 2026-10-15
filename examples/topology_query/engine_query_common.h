/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * Licensed under the CANN Open Software License Agreement Version 2.0.
 * See LICENSE in the root of the software repository for details.
 */
#ifndef SHMEM_EXAMPLES_ENGINE_QUERY_COMMON_H
#define SHMEM_EXAMPLES_ENGINE_QUERY_COMMON_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "acl/acl.h"
#include "host/init/shmem_host_init.h"

namespace shmem_examples {

inline int ParseNonNegativeInt(const char* argument)
{
    const std::string text(argument);
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("PE, NPES and DEVICE must be non-negative integers");
    }
    return std::stoi(text);
}

inline bool CheckStatus(const char* operation, int status, int pe)
{
    if (status != 0) {
        std::cerr << "PE " << pe << ": " << operation << " failed, status=" << status << '\n';
        return false;
    }
    return true;
}

inline bool QueryEngines(int pe, int npes, uint32_t expected)
{
    bool passed = true;
    for (int peer = 0; peer < npes; ++peer) {
        aclshmemx_topology_t info = ACLSHMEMX_TOPOLOGY_INITIALIZER;
        const int status = aclshmemx_query_topology(peer, ACLSHMEMX_TOPOLOGY_QUERY_ENGINES, &info);
        if (!CheckStatus("aclshmemx_query_topology", status, pe)) {
            std::cerr << "  target PE=" << peer << '\n';
            passed = false;
            continue;
        }

        // Print the actual API result. The initialization mask is only our test expectation.
        const uint32_t actual = info.engine_mask;
        std::cout << "PE " << pe << " -> " << peer << " engines=" << actual
                  << " (MTE=" << ((actual & ACLSHMEM_DATA_OP_MTE) != 0)
                  << ", SDMA=" << ((actual & ACLSHMEM_DATA_OP_SDMA) != 0)
                  << ", RoCE=" << ((actual & ACLSHMEM_DATA_OP_ROCE) != 0)
                  << ", UDMA=" << ((actual & ACLSHMEM_DATA_OP_UDMA) != 0) << ")";
        if (peer == pe) {
            // Self reachability follows the library's existing rules, not a remote-link test.
            std::cout << " [SELF: reported only]\n";
        } else {
            const bool matches = actual == expected;
            std::cout << " expected=" << expected << (matches ? " [PASS]\n" : " [FAIL]\n");
            passed = passed && matches;
        }
    }
    return passed;
}

inline int RunEngineQuery(int argc, char** argv, data_op_engine_type_t engines)
{
    if (argc != 5) {
        std::cerr << "Usage: " << argv[0] << " PE NPES DEVICE tcp://HOST:PORT\n"
                  << "Use at least two PEs and run the same example on every PE.\n";
        return EXIT_FAILURE;
    }

    aclshmemx_init_attr_t attr{}; // Retain the public defaults for version and timeouts.
    int device = 0;
    try {
        attr.my_pe = ParseNonNegativeInt(argv[1]);
        attr.n_pes = ParseNonNegativeInt(argv[2]);
        device = ParseNonNegativeInt(argv[3]);
        const std::string ipport(argv[4]);
        if (attr.n_pes < 2 || attr.n_pes > ACLSHMEM_MAX_PES || attr.my_pe >= attr.n_pes) {
            throw std::invalid_argument("NPES must be 2..ACLSHMEM_MAX_PES, and PE must be less than NPES");
        }
        if (ipport.compare(0, 6, "tcp://") != 0 || ipport.size() <= 6 || ipport.size() >= sizeof(attr.ip_port)) {
            throw std::invalid_argument("bootstrap address must fit ip_port and use tcp://HOST:PORT");
        }
        std::memcpy(attr.ip_port, ipport.c_str(), ipport.size() + 1);
    } catch (const std::exception& error) {
        std::cerr << "Invalid arguments: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    attr.local_mem_size = 64ULL * 1024 * 1024;
    attr.option_attr.data_op_engine_type = engines;

    const int pe = attr.my_pe;
    if (!CheckStatus("aclInit", aclInit(nullptr), pe)) {
        return EXIT_FAILURE;
    }
    uint32_t device_count = 0;
    if (!CheckStatus("aclrtGetDeviceCount", aclrtGetDeviceCount(&device_count), pe) ||
        static_cast<uint32_t>(device) >= device_count) {
        std::cerr << "PE " << pe << ": requested logical device=" << device
                  << ", visible device count=" << device_count << '\n';
        CheckStatus("aclFinalize", aclFinalize(), pe);
        return EXIT_FAILURE;
    }
    if (!CheckStatus("aclrtSetDevice", aclrtSetDevice(device), pe)) {
        CheckStatus("aclFinalize", aclFinalize(), pe);
        return EXIT_FAILURE;
    }

    std::cout << "PE " << pe << ": initializing " << attr.n_pes << " PEs, device=" << device
              << ", engine_mask=" << static_cast<uint32_t>(engines) << std::endl;
    bool passed = CheckStatus("aclshmemx_init_attr", aclshmemx_init_attr(ACLSHMEMX_INIT_WITH_DEFAULT, &attr), pe);
    if (passed) {
        passed = QueryEngines(pe, attr.n_pes, static_cast<uint32_t>(engines));
        // All successfully initialized PEs finalize even when their local checks fail.
        if (!CheckStatus("aclshmem_finalize", aclshmem_finalize(), pe)) {
            passed = false;
        }
    }
    if (!CheckStatus("aclrtResetDevice", aclrtResetDevice(device), pe)) {
        passed = false;
    }
    if (!CheckStatus("aclFinalize", aclFinalize(), pe)) {
        passed = false;
    }
    std::cout << "PE " << pe << ": local result " << (passed ? "PASS" : "FAIL") << std::endl;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace shmem_examples
#endif
