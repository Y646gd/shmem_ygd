# coding=utf-8
# -----------------------------------------------------------------------------------------------------------
# Copyright (c) 2025 Huawei Technologies Co., Ltd.
# This program is free software, you can redistribute it and/or modify it under the terms and conditions of
# CANN Open Software License Agreement Version 2.0 (the "License").
# Please refer to the License for details. You may not use this file except in compliance with the License.
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
# INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
# See LICENSE in the root of the software repository for the full text of the License.
# -----------------------------------------------------------------------------------------------------------
#
import shmem as _shmem

# ``import shmem.core`` bypasses shmem.__getattr__, so explicitly enter the
# package's native-library loading stage before any child module imports
# shmem._pyshmem.
_shmem._ensure_native_libraries()

from . import config as _config
from . import init_final as _init_final
from . import memory as _memory
from . import multi_instance as _multi_instance
from . import profiling as _profiling
from . import rma as _rma
from . import sync as _sync
from . import topology as _topology

from .config import *
from .init_final import *
from .memory import *
from .multi_instance import *
from .profiling import *
from .rma import *
from .sync import *
from .topology import *

# Define public exports
__all__ = (
    _init_final.__all__
    + _rma.__all__
    + _memory.__all__
    + _sync.__all__
    + _multi_instance.__all__
    + _config.__all__
    + _profiling.__all__
    + _topology.__all__
)
