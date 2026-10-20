#!/usr/bin/env python
# coding=utf-8
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2026 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------

import shmem._pyshmem as _pyshmem
from shmem.core.utils import AclshmemError, AclshmemInvalid, _instance_lock

__all__ = ['query_topology']


def query_topology(pe: int, *, engines: bool = True, links: bool = False) -> dict:
    """Query the active instance's topology to a world PE without communication.

    ENGINES is supported on all platforms. LINKS requires Ascend950 and returns
    sorted layer IDs (0/1/3), independent of QP policy and engine enablement.
    The returned Python list owns its values and survives instance finalization.
    Only requested keys (engine_mask, net_layers) are present. Topology reflects
    initialization-time descriptions, not live channel health or actual QPs.
    """
    if not isinstance(pe, int) or isinstance(pe, bool) or not -(1 << 31) <= pe < (1 << 31):
        raise AclshmemInvalid("pe must be a signed 32-bit integer.")
    if not isinstance(engines, bool) or not isinstance(links, bool):
        raise AclshmemInvalid("engines and links must be bool values.")
    mask = (0x1 if engines else 0) | (0x2 if links else 0)
    with _instance_lock:
        status, payload = _pyshmem.aclshmemx_query_topology(pe, mask)
        if status != 0:
            raise AclshmemError(f"Query topology failed, ret={status}.")
        return payload
