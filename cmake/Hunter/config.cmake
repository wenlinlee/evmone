# evmone: Fast Ethereum Virtual Machine implementation
# Copyright 2018 The evmone Authors.
# SPDX-License-Identifier: Apache-2.0

include(hunter_cmake_args)

hunter_cmake_args(
    ethash
    CMAKE_ARGS -DETHASH_BUILD_ETHASH=OFF
)

hunter_config(
    intx
    VERSION 0.10.0
    URL https://github.com/chfast/intx/archive/v0.10.1.tar.gz
    SHA1 168e0c2ff6760bd6467e25523cc0e68447358018
)
hunter_config(benchmark
    VERSION 1.5.4
    URL https://github.com/google/benchmark/archive/refs/tags/v1.5.4.tar.gz
    SHA1 b8c2501b33c8649a2c9e2268d8f652284ac5a778
)

hunter_config(evmc VERSION v10.1.0-d2627090
        URL https://github.com/FISCO-BCOS/evmc/archive/d262709028f19e7dcf8cd64856ac77ea8a98d923.tar.gz
        SHA1 e33a8d209eab36735400a909958221b901706f66
)
