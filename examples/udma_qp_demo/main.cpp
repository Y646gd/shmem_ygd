/**
 * Copyright (c) 2026 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "acl/acl.h"
#include "shmem.h"
#include "utils.h"
#ifdef ACLSHMEM_TOPOLOGY_DEMO
#include "../topology_query/topology_plan.h"
#endif

/*
 * 本样例验证直连 UDMA 多 QP 的最小数据通路。
 *
 * 每个 PE 只和环上的下一个 PE 通信。
 *   Put：本 PE 的本地缓冲区写入下一个 PE 的对称内存。
 *   Get：从下一个 PE 的对称内存读取到本 PE 的本地缓冲区。
 *
 * Host 启动与 QP 数量相同的 AIV，每个 AIV 独占一个 QP，
 * 避免多个提交者竞争同一个 SQ。
 */

extern void launch_udma_qp_demo(
    uint32_t block_dim, void* stream, uint8_t* symmetric, uint8_t* local_buffer, uint64_t* signal_words,
    uint64_t elements, int32_t peer, int32_t operation);

namespace {

constexpr uint64_t UDMA_MAX_REQUEST_BYTES = 256ULL * 1024ULL * 1024ULL;

enum class DemoOperation : int32_t {
    PUT = 0,
    GET = 1,
    PUT_SIGNAL = 2,
};

struct Options {
    std::map<uint32_t, uint32_t> qp_net_layers;
    std::vector<int> device_ids;
    uint32_t peer_stride{1};
    int pe{0};            // 当前进程在 SHMEM 通信域中的 PE 编号。
    int pes{2};           // SHMEM 通信域的 PE 总数，至少为 2，避免向本 PE 发送。
    uint32_t qp_count{2}; // 每个远端 PE 创建的 QP 数，同时也是 kernel block 数。
    bool shared_jetty{false}; // 是否启用 shared jetty（通过初始化参数 attr.runtime_config.udma_shared_jetty 设置）。
    uint64_t elements{1024 * 1024};             // 本次传输的 uint8_t 元素数，即传输字节数。
    uint64_t heap_mb{1024};                     // 每个 PE 的 SHMEM 对称堆大小，单位为 MiB。
    int first_npu{0};                           // PE0 使用的逻辑 NPU 编号；PE i 使用 first_npu + i。
    std::string ipport{"tcp://127.0.0.1:8899"}; // 初始化的控制面地址。
    std::string op{"put"};                      // 测试 Put、Get 或 PutSignal，默认只运行 Put。
};

struct QpSlice {
    uint64_t offset{0};
    uint64_t length{0};
};

std::vector<QpSlice> BuildQpSlices(uint64_t elements, uint32_t qp_count)
{
    std::vector<QpSlice> slices(qp_count);
    const uint64_t base_count = elements / qp_count;
    const uint64_t remainder = elements % qp_count;
    for (uint32_t qp_idx = 0; qp_idx < qp_count; ++qp_idx) {
        slices[qp_idx].length = base_count + (qp_idx < remainder ? 1 : 0);
        slices[qp_idx].offset = qp_idx * base_count + std::min<uint64_t>(qp_idx, remainder);
    }
    return slices;
}

void PrintUsage(const char* program)
{
    std::cerr << "Usage: " << program
              << " -pe ID -pes N [-qp_count N] [-shared_jetty 0|1] [-op put|get|put_signal] [-elems N]"
                 " [-heap_mb N] [-first_npu ID] [-device_ids 1,3,2,4 (overrides -first_npu)]"
                 " [-ipport tcp://IP:PORT] [-qp_net_layers 0:0,2:1]"
                 " [-peer_stride N]\n";
}

bool ParseUint64(const char* text, uint64_t& value)
{
    if (text == nullptr || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    value = std::strtoull(text, &end, 10);
    return errno != ERANGE && end != nullptr && *end == '\0';
}

bool ParseQpNetLayers(const char* text, std::map<uint32_t, uint32_t>& net_layers)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    net_layers.clear();
    const std::string value(text);
    size_t begin = 0;
    while (begin < value.size()) {
        const size_t end = value.find(',', begin);
        const std::string entry = value.substr(begin, end - begin);
        const size_t separator = entry.find(':');
        if (separator == std::string::npos || separator == 0 || separator + 1 == entry.size() ||
            entry.find(':', separator + 1) != std::string::npos) {
            return false;
        }
        const std::string qp = entry.substr(0, separator);
        const std::string layer = entry.substr(separator + 1);
        uint64_t qp_idx = 0;
        uint64_t net_layer = 0;
        if (!ParseUint64(qp.c_str(), qp_idx) || qp_idx > std::numeric_limits<uint32_t>::max() ||
            !ParseUint64(layer.c_str(), net_layer) || net_layer > ACLSHMEM_NET_LAYER_3) {
            return false;
        }
        if (!net_layers.emplace(static_cast<uint32_t>(qp_idx), static_cast<uint32_t>(net_layer)).second) {
            std::cerr << "duplicate qp_idx " << qp_idx << " in -qp_net_layers" << std::endl;
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
        if (begin == value.size()) {
            return false;
        }
    }
    return !net_layers.empty();
}

bool ParseDeviceIds(const char* text, std::vector<int>& device_ids)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    device_ids.clear();
    const std::string value(text);
    size_t begin = 0;
    while (begin < value.size()) {
        const size_t end = value.find(',', begin);
        const std::string item = value.substr(begin, end - begin);
        uint64_t device_id = 0;
        if (!ParseUint64(item.c_str(), device_id) ||
            device_id > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        const int parsed_id = static_cast<int>(device_id);
        if (std::find(device_ids.begin(), device_ids.end(), parsed_id) != device_ids.end()) {
            std::cerr << "duplicate device_id " << parsed_id << " in -device_ids" << std::endl;
            return false;
        }
        device_ids.push_back(parsed_id);
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
        if (begin == value.size()) {
            return false;
        }
    }
    return !device_ids.empty();
}

bool ParseOptions(int argc, char** argv, Options& options)
{
    // 所有参数都采用“-参数名 参数值”形式，因此每次处理两个命令行参数。
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) {
            return false;
        }
        const std::string key = argv[i];
        if (key == "-qp_net_layers") {
            if (!ParseQpNetLayers(argv[i + 1], options.qp_net_layers)) {
                return false;
            }
            continue;
        }
        if (key == "-device_ids") {
            if (!ParseDeviceIds(argv[i + 1], options.device_ids)) {
                return false;
            }
            continue;
        }
        uint64_t value = 0;
        if (key == "-op" || key == "-ipport") {
            if (key == "-op") {
                options.op = argv[i + 1];
            } else {
                options.ipport = argv[i + 1];
            }
            continue;
        }
        if (!ParseUint64(argv[i + 1], value)) {
            return false;
        }
        if (key == "-pe") {
            if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                return false;
            }
            options.pe = static_cast<int>(value);
        } else if (key == "-pes") {
            if (value > ACLSHMEM_MAX_PES || value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                return false;
            }
            options.pes = static_cast<int>(value);
        } else if (key == "-qp_count") {
            if (value > ACLSHMEM_MAX_QP_NUM) {
                return false;
            }
            options.qp_count = static_cast<uint32_t>(value);
        } else if (key == "-shared_jetty") {
            if (value > 1) {
                return false;
            }
            options.shared_jetty = value != 0;
        } else if (key == "-peer_stride") {
            if (value == 0 || value > ACLSHMEM_MAX_PES) {
                return false;
            }
            options.peer_stride = static_cast<uint32_t>(value);
        } else if (key == "-elems") {
            options.elements = value;
        } else if (key == "-heap_mb") {
            options.heap_mb = value;
        } else if (key == "-first_npu") {
            if (value > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
                return false;
            }
            options.first_npu = static_cast<int>(value);
        } else {
            return false;
        }
    }
    return true;
}

uint8_t Pattern(int pe, uint64_t index)
{
    return static_cast<uint8_t>((static_cast<uint64_t>(pe + 1) * 37 + index % 251) & 0xFF);
}

std::vector<uint8_t> MakePattern(int pe, uint64_t elements)
{
    std::vector<uint8_t> data(elements);
    for (uint64_t i = 0; i < elements; ++i) {
        data[i] = Pattern(pe, i);
    }
    return data;
}

bool Validate(const std::vector<uint8_t>& actual, int expected_pe, const char* operation, int pe)
{
    for (uint64_t i = 0; i < actual.size(); ++i) {
        const uint8_t expected = Pattern(expected_pe, i);
        if (actual[i] != expected) {
            std::cerr << "[FAIL] op=" << operation << " pe=" << pe << " index=" << i
                      << " actual=" << static_cast<uint32_t>(actual[i])
                      << " expected=" << static_cast<uint32_t>(expected) << std::endl;
            return false;
        }
    }
    std::cout << "[PASS] op=" << operation << " pe=" << pe << " elements=" << actual.size() << std::endl;
    return true;
}

uint64_t SignalValue(int pe, uint32_t qp_idx) { return (static_cast<uint64_t>(pe + 1) << 32) | (qp_idx + 1); }

bool ValidateSignals(const std::vector<uint64_t>& actual, int expected_pe, int pe)
{
    for (uint32_t qp_idx = 0; qp_idx < static_cast<uint32_t>(actual.size()); ++qp_idx) {
        const uint64_t expected = SignalValue(expected_pe, qp_idx);
        if (actual[qp_idx] != expected) {
            std::cerr << "[FAIL] op=put_signal pe=" << pe << " qp_idx=" << qp_idx << " signal=" << actual[qp_idx]
                      << " expected=" << expected << std::endl;
            return false;
        }
    }
    std::cout << "[PASS] op=put_signal pe=" << pe << " signals=" << actual.size() << std::endl;
    return true;
}

struct RuntimeResources {
    int device_id{0};                // 当前 PE 绑定的逻辑设备编号。
    aclrtStream stream{nullptr};     // Host 提交 device kernel 的异步流。
    uint8_t* symmetric{nullptr};     // 可被远端 PE 访问的 SHMEM 对称内存。
    uint64_t* signal_words{nullptr}; // PutSignal 中每个 QP 独占的 SHMEM 对称 signal word。
    uint8_t* local_buffer{nullptr};  // 仅供本 PE 使用的普通 Device 内存。
    bool acl_initialized{false};     // ACL 是否已经初始化。
    bool device_set{false};          // 当前进程是否已经绑定设备。
    bool stream_created{false};      // 异步流是否已经创建。
    bool shmem_initialized{false};   // SHMEM 是否已经初始化。
};

// 记录资源清理阶段的错误；主流程已有错误时保留原始状态，否则记录首个清理错误。
void RecordCleanupError(int cleanup_status, const char* operation, int& status)
{
    if (cleanup_status == 0) {
        return;
    }
    std::cerr << operation << " failed during cleanup, status=" << cleanup_status << std::endl;
    if (status == 0) {
        status = cleanup_status;
    }
}

/*
 * 释放当前 PE 已成功创建的资源。
 *
 * 清理顺序与初始化依赖相反：先释放业务内存，再退出 SHMEM，随后销毁执行流、
 * 复位设备并结束 ACL。每项资源都通过指针或状态标记判断是否已创建，
 * 因此初始化在任意阶段失败时都可以调用本函数。
 */
int CleanupResources(RuntimeResources& resources, int status)
{
    // 先释放依赖 SHMEM 和设备运行环境的两类业务内存。
    if (resources.signal_words != nullptr && resources.shmem_initialized) {
        aclshmem_free(resources.signal_words);
        resources.signal_words = nullptr;
    }
    if (resources.symmetric != nullptr && resources.shmem_initialized) {
        aclshmem_free(resources.symmetric);
        resources.symmetric = nullptr;
    }
    if (resources.local_buffer != nullptr) {
        RecordCleanupError(aclrtFree(resources.local_buffer), "aclrtFree", status);
        resources.local_buffer = nullptr;
    }

    // 再按照 SHMEM、执行流、设备和 ACL 的逆初始化顺序清理运行环境。
    if (resources.shmem_initialized) {
        RecordCleanupError(aclshmem_finalize(), "aclshmem_finalize", status);
        resources.shmem_initialized = false;
    }
    if (resources.stream_created) {
        RecordCleanupError(aclrtDestroyStream(resources.stream), "aclrtDestroyStream", status);
        resources.stream = nullptr;
        resources.stream_created = false;
    }
    if (resources.device_set) {
        RecordCleanupError(aclrtResetDevice(resources.device_id), "aclrtResetDevice", status);
        resources.device_set = false;
    }
    if (resources.acl_initialized) {
        RecordCleanupError(aclFinalize(), "aclFinalize", status);
        resources.acl_initialized = false;
    }
    return status;
}

// 运行阶段失败时先通知所有 PE 退出，避免其他 PE 阻塞，再清理本 PE 已创建的资源。
int HandleRuntimeFailure(RuntimeResources& resources, int status, const char* operation)
{
    std::cerr << operation << " failed, status=" << status << std::endl;
    // 单个 PE 直接返回可能导致其他 PE 阻塞在后续集合通信中，因此先通知所有 PE 退出。
    // 如果全局退出接口返回，则继续清理本 PE 已创建的资源。
    aclshmem_global_exit(status);
    return CleanupResources(resources, status);
}

} // namespace

int main(int argc, char** argv)
{
    // 阶段 1：解析并校验测试参数，本阶段不访问设备或参与跨 PE 集合通信。
    Options options;
#ifdef ACLSHMEM_TOPOLOGY_DEMO
    options.qp_net_layers = {{0, 0}, {1, 1}};
#endif
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 1;
    }
    if (options.pes < 2 || options.pe < 0 || options.pe >= options.pes || options.qp_count == 0 ||
        options.qp_count > ACLSHMEM_MAX_QP_NUM || options.elements < options.qp_count ||
        options.peer_stride >= static_cast<uint32_t>(options.pes) || options.elements > UDMA_MAX_REQUEST_BYTES ||
        (options.op != "put" && options.op != "get" && options.op != "put_signal")) {
        PrintUsage(argv[0]);
        return 1;
    }
    if (!options.device_ids.empty() && options.device_ids.size() != static_cast<size_t>(options.pes)) {
        std::cerr << "-device_ids must contain exactly one unique logical device ID per PE" << std::endl;
        return 1;
    }
    // 后续内存分配和拷贝需要使用实际字节数，因此先排除整数溢出。
    if (options.elements > std::numeric_limits<size_t>::max() ||
        options.heap_mb > std::numeric_limits<uint64_t>::max() / (1024ULL * 1024ULL)) {
        std::cerr << "Size argument overflows this process" << std::endl;
        return 1;
    }
    const uint64_t heap_bytes = options.heap_mb * 1024ULL * 1024ULL;
    const uint64_t signal_bytes = options.qp_count * sizeof(uint64_t);
    const uint64_t required_symmetric_bytes =
        options.op == "put_signal" ? options.elements + signal_bytes : options.elements;
    if (required_symmetric_bytes < options.elements || required_symmetric_bytes > heap_bytes) {
        std::cerr << "-heap_mb must cover all symmetric buffers" << std::endl;
        return 1;
    }

    const uint64_t device_id = options.device_ids.empty() ?
                                   static_cast<uint64_t>(options.first_npu) + static_cast<uint64_t>(options.pe) :
                                   static_cast<uint64_t>(options.device_ids[options.pe]);
    if (device_id > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        std::cerr << "Resolved logical device ID exceeds the supported range" << std::endl;
        return 1;
    }

    RuntimeResources resources;
    resources.device_id = static_cast<int>(device_id);

    // 阶段 2：初始化 ACL、绑定当前 PE 对应的逻辑 NPU，并创建提交 device kernel 的 stream。
    int status = aclInit(nullptr);
    if (status != 0) {
        std::cerr << "aclInit failed, status=" << status << std::endl;
        return status;
    }
    resources.acl_initialized = true;
    status = aclrtSetDevice(resources.device_id);
    if (status != 0) {
        std::cerr << "aclrtSetDevice failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }
    resources.device_set = true;
    status = aclrtCreateStream(&resources.stream);
    if (status != 0) {
        std::cerr << "aclrtCreateStream failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }
    resources.stream_created = true;

    // 阶段 3：构造 SHMEM 初始化参数，并在初始化前设置 UDMA 传输方式和每个远端 PE 的 QP 数。
    aclshmemx_uniqueid_t default_flag_uid{};
    aclshmemx_init_attr_t attr{};
    status = test_set_attr(options.pe, options.pes, heap_bytes, options.ipport.c_str(), default_flag_uid, &attr);
    if (status != 0) {
        std::cerr << "test_set_attr failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }
    // 所有 PE 必须由启动器保证配置相同的 QP 数。
    attr.option_attr.data_op_engine_type = ACLSHMEM_DATA_OP_UDMA;
    status = aclshmemx_set_qp_num(ACLSHMEM_DATA_OP_UDMA, options.qp_count);
    if (status != 0) {
        std::cerr << "aclshmemx_set_qp_num failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }
    if (options.shared_jetty) {
        // 所有 PE 必须由启动器保证相同的 shared jetty 配置。
        attr.runtime_config.udma_shared_jetty = ACLSHMEM_CONFIG_ENABLED;
    }
    if (!options.qp_net_layers.empty()) {
        if (options.qp_net_layers.rbegin()->first >= options.qp_count) {
            std::cerr << "qp_net_layers contains qp_idx outside [0, qp_count)" << std::endl;
            return CleanupResources(resources, 1);
        }
        for (const auto& [qp_idx, net_layer] : options.qp_net_layers) {
            status = aclshmemx_set_qp_net_layer(
                ACLSHMEM_DATA_OP_UDMA, qp_idx, static_cast<aclshmemx_net_layer_t>(net_layer));
            if (status != 0) {
                std::cerr << "aclshmemx_set_qp_net_layer failed, qp_idx=" << qp_idx << ", status=" << status
                          << std::endl;
                return CleanupResources(resources, status);
            }
        }
    }
    status = aclshmemx_init_attr(ACLSHMEMX_INIT_WITH_DEFAULT, &attr);
    if (status != 0) {
        std::cerr << "aclshmemx_init_attr failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }
    resources.shmem_initialized = true;
    const int next_pe = (options.pe + static_cast<int>(options.peer_stride)) % options.pes;
#ifdef ACLSHMEM_TOPOLOGY_DEMO
    status = VerifyTwoLayerPlan(next_pe);
    if (status != ACLSHMEM_SUCCESS || options.qp_count != 2) {
        std::cerr << "Two-layer demo needs two established UDMA QPs on layers 0 and 1, status=" << status << '\n';
        // Stop every member before any peer launches its data transfer.
        aclshmem_global_exit(status != 0 ? status : ACLSHMEM_INVALID_VALUE);
        return CleanupResources(resources, 1);
    }
#endif
    const std::vector<QpSlice> qp_slices = BuildQpSlices(options.elements, options.qp_count);
    for (uint32_t qp_idx = 0; qp_idx < options.qp_count; ++qp_idx) {
        aclshmemx_net_layer_t net_layer = ACLSHMEM_NET_LAYER_0;
        status = aclshmemx_get_qp_net_layer(ACLSHMEM_DATA_OP_UDMA, next_pe, qp_idx, &net_layer);
        if (status != 0) {
            std::cerr << "aclshmemx_get_qp_net_layer failed, qp_idx=" << qp_idx << ", status=" << status << std::endl;
            return CleanupResources(resources, status);
        }
        const auto policy_it = options.qp_net_layers.find(qp_idx);
        std::ostringstream route_log;
        route_log << "[QP-ROUTE] src_pe=" << options.pe << " dst_pe=" << next_pe << " qp=" << qp_idx << " policy=";
        if (policy_it == options.qp_net_layers.end()) {
            route_log << "default";
        } else {
            route_log << "explicit:" << policy_it->second;
        }
        route_log << " actual_layer=" << static_cast<uint32_t>(net_layer) << " offset=" << qp_slices[qp_idx].offset
                  << " length=" << qp_slices[qp_idx].length;
        std::cout << route_log.str() << std::endl;
    }
    /*
     * 阶段 4：分配两类用途不同的设备内存。
     * 对称内存由 aclshmem_malloc 分配，各 PE 按相同顺序和大小创建，可被远端 PE 访问。
     * 本地缓冲区由 aclrtMalloc 分配，只属于本 PE，用作 Put 的本地源或 Get 的本地目标。
     */
    resources.symmetric = static_cast<uint8_t*>(aclshmem_malloc(static_cast<size_t>(options.elements)));
    if (resources.symmetric == nullptr) {
        std::cerr << "aclshmem_malloc failed" << std::endl;
        return CleanupResources(resources, 1);
    }
    if (options.op == "put_signal") {
        resources.signal_words = static_cast<uint64_t*>(aclshmem_malloc(static_cast<size_t>(signal_bytes)));
        if (resources.signal_words == nullptr) {
            std::cerr << "aclshmem_malloc signal_words failed" << std::endl;
            return CleanupResources(resources, 1);
        }
    }
    status = aclrtMalloc(
        reinterpret_cast<void**>(&resources.local_buffer), static_cast<size_t>(options.elements),
        ACL_MEM_MALLOC_HUGE_FIRST);
    if (status != 0) {
        std::cerr << "aclrtMalloc failed, status=" << status << std::endl;
        return CleanupResources(resources, status);
    }

    // 阶段 5：准备校验数据和环形拓扑。
    const std::vector<uint8_t> own_pattern = MakePattern(options.pe, options.elements);
    std::vector<uint8_t> result(options.elements);
    const int previous_pe = (options.pe + options.pes - static_cast<int>(options.peer_stride)) % options.pes;
    // 记录当前 PE 的校验结果，任一数据校验或运行状态失败时都会标记为失败。
    bool passed = true;

    if (options.op == "put" || options.op == "put_signal") {
        status = aclrtMemset(resources.symmetric, options.elements, 0, options.elements);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemset");
        }
        status = aclrtMemcpy(
            resources.local_buffer, options.elements, own_pattern.data(), options.elements, ACL_MEMCPY_HOST_TO_DEVICE);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemcpy H2D");
        }
        if (options.op == "put_signal") {
            status = aclrtMemset(resources.signal_words, signal_bytes, 0, signal_bytes);
            if (status != 0) {
                return HandleRuntimeFailure(resources, status, "aclrtMemset signal_words");
            }
        }
        aclshmem_barrier_all();
        const DemoOperation operation = options.op == "put" ? DemoOperation::PUT : DemoOperation::PUT_SIGNAL;
        launch_udma_qp_demo(
            options.qp_count, resources.stream, resources.symmetric, resources.local_buffer, resources.signal_words,
            options.elements, next_pe, static_cast<int32_t>(operation));
        status = aclrtSynchronizeStream(resources.stream);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtSynchronizeStream");
        }
        aclshmem_barrier_all();
        status = aclrtMemcpy(
            result.data(), options.elements, resources.symmetric, options.elements, ACL_MEMCPY_DEVICE_TO_HOST);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemcpy D2H");
        }
        passed = Validate(result, previous_pe, options.op.c_str(), options.pe) && passed;
        if (options.op == "put_signal") {
            std::vector<uint64_t> signals(options.qp_count);
            status = aclrtMemcpy(
                signals.data(), signal_bytes, resources.signal_words, signal_bytes, ACL_MEMCPY_DEVICE_TO_HOST);
            if (status != 0) {
                return HandleRuntimeFailure(resources, status, "aclrtMemcpy signal_words D2H");
            }
            passed = ValidateSignals(signals, previous_pe, options.pe) && passed;
        }
    } else {
        status = aclrtMemcpy(
            resources.symmetric, options.elements, own_pattern.data(), options.elements, ACL_MEMCPY_HOST_TO_DEVICE);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemcpy H2D");
        }
        status = aclrtMemset(resources.local_buffer, options.elements, 0, options.elements);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemset");
        }
        aclshmem_barrier_all();
        launch_udma_qp_demo(
            options.qp_count, resources.stream, resources.symmetric, resources.local_buffer, nullptr, options.elements,
            next_pe, static_cast<int32_t>(DemoOperation::GET));
        status = aclrtSynchronizeStream(resources.stream);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtSynchronizeStream");
        }
        aclshmem_barrier_all();
        status = aclrtMemcpy(
            result.data(), options.elements, resources.local_buffer, options.elements, ACL_MEMCPY_DEVICE_TO_HOST);
        if (status != 0) {
            return HandleRuntimeFailure(resources, status, "aclrtMemcpy D2H");
        }
        passed = Validate(result, next_pe, "get", options.pe) && passed;
    }

    // 阶段 6：按照业务内存、SHMEM、ACL 运行时的依赖顺序释放资源，并合并最终状态。
    status = CleanupResources(resources, status);
    return (status == 0 && passed) ? 0 : 1;
}
