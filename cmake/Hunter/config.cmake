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

hunter_config(evmc VERSION v11.0.1-b2986bdd
        URL https://github.com/FISCO-BCOS/evmc/archive/b2986bdd68ddf9f35c3ae889c5bee8b643ecb4bd.tar.gz
        SHA1 31d90cc44332d15b12e375cfd13402ddfe715b47
)
