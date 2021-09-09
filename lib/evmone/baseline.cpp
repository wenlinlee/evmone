// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2020 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "baseline.hpp"
#include "baseline_instruction_table.hpp"
#include "execution_state.hpp"
#include "instructions.hpp"
#include "instructions_op2fn.hpp"
#include "vm.hpp"
#include <evmc/instructions.h>
#include <memory>

namespace evmone::baseline
{
CodeAnalysis analyze(const uint8_t* code, size_t code_size)
{
    // To find if op is any PUSH opcode (OP_PUSH1 <= op <= OP_PUSH32)
    // it can be noticed that OP_PUSH32 is INT8_MAX (0x7f) therefore
    // static_cast<int8_t>(op) <= OP_PUSH32 is always true and can be skipped.
    static_assert(OP_PUSH32 == std::numeric_limits<int8_t>::max());

    CodeAnalysis::JumpdestMap map(code_size);  // Allocate and init bitmap with zeros.
    size_t i = 0;
    while (i < code_size)
    {
        const auto op = code[i];
        if (static_cast<int8_t>(op) >= OP_PUSH1)  // If any PUSH opcode (see explanation above).
            i += op - size_t{OP_PUSH1 - 1};       // Skip PUSH data.
        else if (INTX_UNLIKELY(op == OP_JUMPDEST))
            map[i] = true;
        ++i;
    }

    // i is the needed code size including the last push data (can be bigger than code_size).
    // Using "raw" new operator instead of std::make_unique() to get uninitialized array.
    std::unique_ptr<uint8_t[]> padded_code{new uint8_t[i + 1]};  // +1 for the final STOP.
    std::copy_n(code, code_size, padded_code.get());
    padded_code[code_size] = OP_STOP;  // Used to terminate invalid jumps, see op_jump().
    padded_code[i] = OP_STOP;  // Set final STOP at the code end - guarantees loop termination.

    // TODO: Using fixed-size padding of 33, the padded code buffer and jumpdest bitmap can be
    //       created with single allocation.

    return CodeAnalysis{std::move(padded_code), std::move(map)};
}

namespace
{
inline evmc_status_code check_requirements(
    const InstructionTable& instruction_table, ExecutionState& state, uint8_t op) noexcept
{
    const auto metrics = instruction_table[op];

    if (INTX_UNLIKELY(metrics.gas_cost == instr::undefined))
        return EVMC_UNDEFINED_INSTRUCTION;

    if (INTX_UNLIKELY((state.gas_left -= metrics.gas_cost) < 0))
        return EVMC_OUT_OF_GAS;

    const auto stack_size = state.stack.size();
    if (INTX_UNLIKELY(stack_size == Stack::limit))
    {
        if (metrics.can_overflow_stack)
            return EVMC_STACK_OVERFLOW;
    }
    else if (INTX_UNLIKELY(stack_size < metrics.stack_height_required))
        return EVMC_STACK_UNDERFLOW;

    return EVMC_SUCCESS;
}

/// Dispatch the instruction currently pointed by "pc".
#define DISPATCH() break  // Break out of switch statement.

/// Increment "pc" and dispatch the instruction.
#define DISPATCH_NEXT() \
    ++code_it;          \
    DISPATCH()

/// Implementation of a generic instruction "case".
#define DISPATCH_CASE(OPCODE)                                          \
    case OPCODE:                                                       \
        if (code_it = invoke(op2fn::OPCODE, state, code_it); !code_it) \
            goto exit;                                                 \
        break

/// The signature of basic instructions which always succeed, e.g. ADD.
using SucceedingInstrFn = void(ExecutionState&) noexcept;
static_assert(std::is_same_v<decltype(add), SucceedingInstrFn>);

/// The signature of basic instructions which may fail.
using MayFailInstrFn = evmc_status_code(ExecutionState&) noexcept;
static_assert(std::is_same_v<decltype(exp), MayFailInstrFn>);

/// The signature of terminating instructions.
using TerminatingInstrFn = StopToken(ExecutionState&) noexcept;
static_assert(std::is_same_v<decltype(stop), TerminatingInstrFn>);

/// The signature of instructions requiring access to current code position.
using CodePositionInstrFn = code_iterator(ExecutionState&, code_iterator) noexcept;
static_assert(std::is_same_v<decltype(push<1>), CodePositionInstrFn>);
static_assert(std::is_same_v<decltype(pc), CodePositionInstrFn>);
static_assert(std::is_same_v<decltype(jump), CodePositionInstrFn>);

/// A helper to invoke instruction implementations of different signatures
/// done by template specialization.
template <typename InstrFn>
code_iterator invoke(InstrFn instr_fn, ExecutionState& state, code_iterator pos) noexcept = delete;

template <>
[[gnu::always_inline]] inline code_iterator invoke<SucceedingInstrFn*>(
    SucceedingInstrFn* instr_fn, ExecutionState& state, code_iterator pos) noexcept
{
    instr_fn(state);
    return pos + 1;
}

template <>
[[gnu::always_inline]] inline code_iterator invoke<MayFailInstrFn*>(
    MayFailInstrFn* instr_fn, ExecutionState& state, code_iterator pos) noexcept
{
    if (const auto status = instr_fn(state); status != EVMC_SUCCESS)
    {
        state.status = status;
        return nullptr;
    }
    return pos + 1;
}

template <>
[[gnu::always_inline]] inline code_iterator invoke<TerminatingInstrFn*>(
    TerminatingInstrFn* instr_fn, ExecutionState& state, code_iterator /*pos*/) noexcept
{
    state.status = instr_fn(state).status;
    return nullptr;
}

template <>
[[gnu::always_inline]] inline code_iterator invoke<CodePositionInstrFn*>(
    CodePositionInstrFn* instr_fn, ExecutionState& state, code_iterator pos) noexcept
{
    return instr_fn(state, pos);
}

template <bool TracingEnabled>
evmc_result execute(const VM& vm, ExecutionState& state, const CodeAnalysis& analysis) noexcept
{
    state.analysis.baseline = &analysis;  // Assign code analysis for instruction implementations.

    // Use padded code.
    state.code = {analysis.padded_code.get(), state.code.size()};

    auto* tracer = vm.get_tracer();
    if constexpr (TracingEnabled)
        tracer->notify_execution_start(state.rev, *state.msg, state.code);

    const auto& instruction_table = get_baseline_instruction_table(state.rev);

    const auto* const code = state.code.data();
    auto code_it = code;  // Code iterator for the interpreter loop.
    while (true)          // Guaranteed to terminate because padded code ends with STOP.
    {
        if constexpr (TracingEnabled)
        {
            const auto offset = static_cast<uint32_t>(code_it - code);
            if (offset < state.code.size())  // Skip STOP from code padding.
                tracer->notify_instruction_start(offset, state);
        }

        const auto op = *code_it;
        if (const auto status = check_requirements(instruction_table, state, op);
            status != EVMC_SUCCESS)
        {
            state.status = status;
            goto exit;
        }

        switch (op)
        {
            DISPATCH_CASE(OP_STOP);
            DISPATCH_CASE(OP_ADD);
            DISPATCH_CASE(OP_MUL);
            DISPATCH_CASE(OP_SUB);
            DISPATCH_CASE(OP_DIV);
            DISPATCH_CASE(OP_SDIV);
            DISPATCH_CASE(OP_MOD);
            DISPATCH_CASE(OP_SMOD);
            DISPATCH_CASE(OP_ADDMOD);
            DISPATCH_CASE(OP_MULMOD);
            DISPATCH_CASE(OP_EXP);

        case OP_SIGNEXTEND:
            signextend(state);
            DISPATCH_NEXT();

        case OP_LT:
            lt(state);
            DISPATCH_NEXT();
        case OP_GT:
            gt(state);
            DISPATCH_NEXT();
        case OP_SLT:
            slt(state);
            DISPATCH_NEXT();
        case OP_SGT:
            sgt(state);
            DISPATCH_NEXT();
        case OP_EQ:
            eq(state);
            DISPATCH_NEXT();
        case OP_ISZERO:
            iszero(state);
            DISPATCH_NEXT();
        case OP_AND:
            and_(state);
            DISPATCH_NEXT();
        case OP_OR:
            or_(state);
            DISPATCH_NEXT();
        case OP_XOR:
            xor_(state);
            DISPATCH_NEXT();
        case OP_NOT:
            not_(state);
            DISPATCH_NEXT();
        case OP_BYTE:
            byte(state);
            DISPATCH_NEXT();
        case OP_SHL:
            shl(state);
            DISPATCH_NEXT();
        case OP_SHR:
            shr(state);
            DISPATCH_NEXT();
        case OP_SAR:
            sar(state);
            DISPATCH_NEXT();

        case OP_KECCAK256:
        {
            const auto status_code = keccak256(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }

        case OP_ADDRESS:
            address(state);
            DISPATCH_NEXT();
        case OP_BALANCE:
        {
            const auto status_code = balance(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_ORIGIN:
            origin(state);
            DISPATCH_NEXT();
        case OP_CALLER:
            caller(state);
            DISPATCH_NEXT();
        case OP_CALLVALUE:
            callvalue(state);
            DISPATCH_NEXT();
        case OP_CALLDATALOAD:
            calldataload(state);
            DISPATCH_NEXT();
        case OP_CALLDATASIZE:
            calldatasize(state);
            DISPATCH_NEXT();
        case OP_CALLDATACOPY:
        {
            const auto status_code = calldatacopy(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_CODESIZE:
            codesize(state);
            DISPATCH_NEXT();
        case OP_CODECOPY:
        {
            const auto status_code = codecopy(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_GASPRICE:
            gasprice(state);
            DISPATCH_NEXT();
        case OP_EXTCODESIZE:
        {
            const auto status_code = extcodesize(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_EXTCODECOPY:
        {
            const auto status_code = extcodecopy(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_RETURNDATASIZE:
            returndatasize(state);
            DISPATCH_NEXT();
        case OP_RETURNDATACOPY:
        {
            const auto status_code = returndatacopy(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_EXTCODEHASH:
        {
            const auto status_code = extcodehash(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_BLOCKHASH:
            blockhash(state);
            DISPATCH_NEXT();
        case OP_COINBASE:
            coinbase(state);
            DISPATCH_NEXT();
        case OP_TIMESTAMP:
            timestamp(state);
            DISPATCH_NEXT();
        case OP_NUMBER:
            number(state);
            DISPATCH_NEXT();
        case OP_DIFFICULTY:
            difficulty(state);
            DISPATCH_NEXT();
        case OP_GASLIMIT:
            gaslimit(state);
            DISPATCH_NEXT();
        case OP_CHAINID:
            chainid(state);
            DISPATCH_NEXT();
        case OP_SELFBALANCE:
            selfbalance(state);
            DISPATCH_NEXT();
        case OP_BASEFEE:
            basefee(state);
            DISPATCH_NEXT();

        case OP_POP:
            pop(state);
            DISPATCH_NEXT();
        case OP_MLOAD:
        {
            const auto status_code = mload(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_MSTORE:
        {
            const auto status_code = mstore(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_MSTORE8:
        {
            const auto status_code = mstore8(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }

            DISPATCH_CASE(OP_JUMP);
            DISPATCH_CASE(OP_JUMPI);
            DISPATCH_CASE(OP_PC);

        case OP_MSIZE:
            msize(state);
            DISPATCH_NEXT();
        case OP_SLOAD:
        {
            const auto status_code = sload(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_SSTORE:
        {
            const auto status_code = sstore(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_GAS:
            gas(state);
            DISPATCH_NEXT();
        case OP_JUMPDEST:
            jumpdest(state);
            DISPATCH_NEXT();

            DISPATCH_CASE(OP_PUSH1);
            DISPATCH_CASE(OP_PUSH2);
            DISPATCH_CASE(OP_PUSH3);
            DISPATCH_CASE(OP_PUSH4);
            DISPATCH_CASE(OP_PUSH5);
            DISPATCH_CASE(OP_PUSH6);
            DISPATCH_CASE(OP_PUSH7);
            DISPATCH_CASE(OP_PUSH8);
            DISPATCH_CASE(OP_PUSH9);
            DISPATCH_CASE(OP_PUSH10);
            DISPATCH_CASE(OP_PUSH11);
            DISPATCH_CASE(OP_PUSH12);
            DISPATCH_CASE(OP_PUSH13);
            DISPATCH_CASE(OP_PUSH14);
            DISPATCH_CASE(OP_PUSH15);
            DISPATCH_CASE(OP_PUSH16);
            DISPATCH_CASE(OP_PUSH17);
            DISPATCH_CASE(OP_PUSH18);
            DISPATCH_CASE(OP_PUSH19);
            DISPATCH_CASE(OP_PUSH20);
            DISPATCH_CASE(OP_PUSH21);
            DISPATCH_CASE(OP_PUSH22);
            DISPATCH_CASE(OP_PUSH23);
            DISPATCH_CASE(OP_PUSH24);
            DISPATCH_CASE(OP_PUSH25);
            DISPATCH_CASE(OP_PUSH26);
            DISPATCH_CASE(OP_PUSH27);
            DISPATCH_CASE(OP_PUSH28);
            DISPATCH_CASE(OP_PUSH29);
            DISPATCH_CASE(OP_PUSH30);
            DISPATCH_CASE(OP_PUSH31);
            DISPATCH_CASE(OP_PUSH32);

        case OP_DUP1:
            dup<1>(state);
            DISPATCH_NEXT();
        case OP_DUP2:
            dup<2>(state);
            DISPATCH_NEXT();
        case OP_DUP3:
            dup<3>(state);
            DISPATCH_NEXT();
        case OP_DUP4:
            dup<4>(state);
            DISPATCH_NEXT();
        case OP_DUP5:
            dup<5>(state);
            DISPATCH_NEXT();
        case OP_DUP6:
            dup<6>(state);
            DISPATCH_NEXT();
        case OP_DUP7:
            dup<7>(state);
            DISPATCH_NEXT();
        case OP_DUP8:
            dup<8>(state);
            DISPATCH_NEXT();
        case OP_DUP9:
            dup<9>(state);
            DISPATCH_NEXT();
        case OP_DUP10:
            dup<10>(state);
            DISPATCH_NEXT();
        case OP_DUP11:
            dup<11>(state);
            DISPATCH_NEXT();
        case OP_DUP12:
            dup<12>(state);
            DISPATCH_NEXT();
        case OP_DUP13:
            dup<13>(state);
            DISPATCH_NEXT();
        case OP_DUP14:
            dup<14>(state);
            DISPATCH_NEXT();
        case OP_DUP15:
            dup<15>(state);
            DISPATCH_NEXT();
        case OP_DUP16:
            dup<16>(state);
            DISPATCH_NEXT();

        case OP_SWAP1:
            swap<1>(state);
            DISPATCH_NEXT();
        case OP_SWAP2:
            swap<2>(state);
            DISPATCH_NEXT();
        case OP_SWAP3:
            swap<3>(state);
            DISPATCH_NEXT();
        case OP_SWAP4:
            swap<4>(state);
            DISPATCH_NEXT();
        case OP_SWAP5:
            swap<5>(state);
            DISPATCH_NEXT();
        case OP_SWAP6:
            swap<6>(state);
            DISPATCH_NEXT();
        case OP_SWAP7:
            swap<7>(state);
            DISPATCH_NEXT();
        case OP_SWAP8:
            swap<8>(state);
            DISPATCH_NEXT();
        case OP_SWAP9:
            swap<9>(state);
            DISPATCH_NEXT();
        case OP_SWAP10:
            swap<10>(state);
            DISPATCH_NEXT();
        case OP_SWAP11:
            swap<11>(state);
            DISPATCH_NEXT();
        case OP_SWAP12:
            swap<12>(state);
            DISPATCH_NEXT();
        case OP_SWAP13:
            swap<13>(state);
            DISPATCH_NEXT();
        case OP_SWAP14:
            swap<14>(state);
            DISPATCH_NEXT();
        case OP_SWAP15:
            swap<15>(state);
            DISPATCH_NEXT();
        case OP_SWAP16:
            swap<16>(state);
            DISPATCH_NEXT();

        case OP_LOG0:
        {
            const auto status_code = log<0>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_LOG1:
        {
            const auto status_code = log<1>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_LOG2:
        {
            const auto status_code = log<2>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_LOG3:
        {
            const auto status_code = log<3>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_LOG4:
        {
            const auto status_code = log<4>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }

        case OP_CREATE:
        {
            const auto status_code = create<EVMC_CREATE>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_CALL:
        {
            const auto status_code = call<EVMC_CALL>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_CALLCODE:
        {
            const auto status_code = call<EVMC_CALLCODE>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }

            DISPATCH_CASE(OP_RETURN);

        case OP_DELEGATECALL:
        {
            const auto status_code = call<EVMC_DELEGATECALL>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_STATICCALL:
        {
            const auto status_code = call<EVMC_CALL, true>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }
        case OP_CREATE2:
        {
            const auto status_code = create<EVMC_CREATE2>(state);
            if (status_code != EVMC_SUCCESS)
            {
                state.status = status_code;
                goto exit;
            }
            DISPATCH_NEXT();
        }

            DISPATCH_CASE(OP_REVERT);
            DISPATCH_CASE(OP_INVALID);
            DISPATCH_CASE(OP_SELFDESTRUCT);

        default:
            INTX_UNREACHABLE();
        }
    }

exit:
    const auto gas_left =
        (state.status == EVMC_SUCCESS || state.status == EVMC_REVERT) ? state.gas_left : 0;

    const auto result = evmc::make_result(state.status, gas_left,
        state.output_size != 0 ? &state.memory[state.output_offset] : nullptr, state.output_size);

    if constexpr (TracingEnabled)
        tracer->notify_execution_end(result);

    return result;
}
}  // namespace

evmc_result execute(const VM& vm, ExecutionState& state, const CodeAnalysis& analysis) noexcept
{
    if (INTX_UNLIKELY(vm.get_tracer() != nullptr))
        return execute<true>(vm, state, analysis);

    return execute<false>(vm, state, analysis);
}

evmc_result execute(evmc_vm* c_vm, const evmc_host_interface* host, evmc_host_context* ctx,
    evmc_revision rev, const evmc_message* msg, const uint8_t* code, size_t code_size) noexcept
{
    auto vm = static_cast<VM*>(c_vm);
    const auto jumpdest_map = analyze(code, code_size);
    auto state = std::make_unique<ExecutionState>(*msg, rev, *host, ctx, code, code_size);
    return execute(*vm, *state, jumpdest_map);
}
}  // namespace evmone::baseline
