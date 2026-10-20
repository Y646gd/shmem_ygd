#!/usr/bin/env python3
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# Licensed under the CANN Open Software License Agreement Version 2.0.
"""Compile real Host planner and extracted Host/kernel slice arithmetic; resource APIs mocked."""
import argparse
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--gtest', type=Path, default=Path('/tmp/shmem-gtest-complete/googletest-1.14.0'))
p.add_argument('--output', type=Path)
a = p.parse_args()

def body(path, name):
    s = (ROOT/path).read_text(); start = s.index(name); end = s.index('{', start) + 1; depth = 1
    while depth:
        depth += (s[end] == '{') - (s[end] == '}'); end += 1
    return s[start:end]

with tempfile.TemporaryDirectory(prefix='shmem-rfc-planner-') as folder:
    temp = Path(folder)
    (temp/'acl').mkdir()
    (temp/'acl/acl_rt.h').write_text('#pragma once\n#include <cstdint>\n#include <cstddef>\n')
    (temp/'shmem.h').write_text('#pragma once\n#include "host/init/shmem_host_init.h"\n')
    fixture = '#pragma once\n#include <algorithm>\n#include <cstdint>\n#include <vector>\n'
    fixture += body('examples/udma_qp_demo/main.cpp', 'struct QpSlice {') + ';\n'
    fixture += body('examples/udma_qp_demo/main.cpp', 'std::vector<QpSlice> BuildQpSlices(') + '\n'
    kernel = (ROOT/'examples/udma_qp_demo/udma_qp_demo_kernel.cpp').read_text()
    start = kernel.index('    const uint32_t qp_idx ='); end = kernel.index('    AscendC::TPipe pipe;', start)
    fixture += '''namespace AscendC {
inline uint32_t block_index = 0, block_count = 1;
inline uint32_t GetBlockIdx() { return block_index; }
inline uint32_t GetBlockNum() { return block_count; }
}
QpSlice KernelSliceForTest(uint64_t elements, uint32_t index, uint32_t count) {
AscendC::block_index = index; AscendC::block_count = count;
''' + kernel[start:end] + 'return QpSlice{slice_offset, slice_count};\n}\n'
    (temp/'planner_fixture.h').write_text(fixture)
    g = a.gtest.resolve()/'googletest'
    binary = temp/'planner'
    subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-pthread',
                    '-iquote', str(ROOT), *['-I'+str(x) for x in [temp, ROOT/'include', g/'include', g]],
                    str(ROOT/'tests/topology/planner_test.cpp'), str(g/'src/gtest-all.cc'),
                    str(g/'src/gtest_main.cc'), '-o', str(binary)], check=True)
    command = [str(binary)]
    if a.output:
        a.output = a.output.resolve(); a.output.mkdir(parents=True, exist_ok=True)
        command.append('--gtest_output=xml:'+str(a.output/'planner.xml'))
    subprocess.run(command, check=True)
