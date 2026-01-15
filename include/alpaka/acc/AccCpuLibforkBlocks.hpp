/* Copyright 2025 Axel Huebl, Benjamin Worpitz, Erik Zenker, René Widera, Jan Stephan, Bernhard Manfred Gruber,
 *                Andrea Bocci
 * SPDX-License-Identifier: MPL-2.0
 */

#pragma once

// Base classes.
#include "alpaka/atomic/AtomicCpu.hpp"
#include "alpaka/atomic/AtomicHierarchy.hpp"
#include "alpaka/atomic/AtomicNoOp.hpp"
#include "alpaka/block/shared/dyn/BlockSharedMemDynMember.hpp"
#include "alpaka/block/shared/st/BlockSharedMemStMember.hpp"
#include "alpaka/block/sync/BlockSyncNoOp.hpp"
#include "alpaka/core/DemangleTypeNames.hpp"
#include "alpaka/idx/bt/IdxBtZero.hpp"
#include "alpaka/idx/gb/IdxGbRef.hpp"
#include "alpaka/intrinsic/IntrinsicCpu.hpp"
#include "alpaka/math/MathStdLib.hpp"
#include "alpaka/mem/fence/MemFenceCpuSerial.hpp"
#include "alpaka/rand/RandDefault.hpp"
#include "alpaka/rand/RandStdLib.hpp"
#include "alpaka/warp/WarpSingleThread.hpp"
#include "alpaka/workdiv/WorkDivMembers.hpp"

// Specialized traits.
#include "alpaka/acc/Traits.hpp"
#include "alpaka/dev/Traits.hpp"
#include "alpaka/idx/Traits.hpp"
#include "alpaka/kernel/Traits.hpp"
#include "alpaka/platform/Traits.hpp"

// Implementation details.
#include "alpaka/acc/Tag.hpp"
#include "alpaka/core/Interface.hpp"
#include "alpaka/dev/DevCpu.hpp"

#ifdef __cpp_lib_format
#    include <format>
#endif
#include <memory>
#include <thread>
#include <limits>
#include <string>

#ifdef ALPAKA_ACC_CPU_B_LIBFORK_T_SEQ_ENABLED

namespace alpaka
{
    // Forward declaration
    template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
    class TaskKernelCpuLibforkBlocks;

    //! The CPU Libfork block accelerator.
    template<typename TDim, typename TIdx>
    class AccCpuLibforkBlocks final
        : public WorkDivMembers<TDim, TIdx>
        , public gb::IdxGbRef<TDim, TIdx>
        , public bt::IdxBtZero<TDim, TIdx>
        , public AtomicHierarchy<
              AtomicCpu,
              AtomicNoOp,
              AtomicNoOp>
        , public math::MathStdLib
        , public BlockSharedMemDynMember<>
        , public BlockSharedMemStMember<>
        , public BlockSyncNoOp
        , public IntrinsicCpu
        , public MemFenceCpu
#    ifdef ALPAKA_DISABLE_VENDOR_RNG
        , public rand::RandDefault
#    else
        , public rand::RandStdLib
#    endif
        , public warp::WarpSingleThread
        , public interface::Implements<InterfaceAcc, AccCpuLibforkBlocks<TDim, TIdx>>
    {
        static_assert(
            sizeof(TIdx) >= sizeof(int),
            "Index type is not supported, consider using int or a larger type.");

    public:
        // Partial specialization with the correct TDim and TIdx is not allowed.
        template<typename TDim2, typename TIdx2, typename TKernelFnObj, typename... TArgs>
        friend class ::alpaka::TaskKernelCpuLibforkBlocks;

        AccCpuLibforkBlocks(AccCpuLibforkBlocks const&) = delete;
        AccCpuLibforkBlocks(AccCpuLibforkBlocks&&) = delete;
        auto operator=(AccCpuLibforkBlocks const&) -> AccCpuLibforkBlocks& = delete;
        auto operator=(AccCpuLibforkBlocks&&) -> AccCpuLibforkBlocks& = delete;

    protected:
        template<typename TWorkDiv>
        ALPAKA_FN_HOST AccCpuLibforkBlocks(TWorkDiv const& workDiv, std::size_t const& blockSharedMemDynSizeBytes)
            : WorkDivMembers<TDim, TIdx>(workDiv)
            , gb::IdxGbRef<TDim, TIdx>(m_gridBlockIdx)
            , BlockSharedMemDynMember<>(blockSharedMemDynSizeBytes)
            , BlockSharedMemStMember<>(staticMemBegin(), staticMemCapacity())
            , m_gridBlockIdx(Vec<TDim, TIdx>::zeros())
        {
        }


        Vec<TDim, TIdx> mutable m_gridBlockIdx; //!< The index of the currently executed block.
    };

    namespace trait
    {
        template<typename TDim, typename TIdx>
        struct AccType<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = AccCpuLibforkBlocks<TDim, TIdx>;
        };

        template<typename TDim, typename TIdx>
        struct GetAccDevProps<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            ALPAKA_FN_HOST static auto getAccDevProps(DevCpu const& dev) -> AccDevProps<TDim, TIdx>
            {
                (void)dev; // suppress unused parameter warning
                return {// m_multiProcessorCount
                        static_cast<TIdx>(std::thread::hardware_concurrency()),
                        // m_gridBlockExtentMax
                        Vec<TDim, TIdx>::all(std::numeric_limits<TIdx>::max()),
                        // m_gridBlockCountMax
                        std::numeric_limits<TIdx>::max(),
                        // m_blockThreadExtentMax
                        Vec<TDim, TIdx>::ones(),
                        // m_blockThreadCountMax
                        static_cast<TIdx>(1),
                        // m_threadElemExtentMax
                        Vec<TDim, TIdx>::all(std::numeric_limits<TIdx>::max()),
                        // m_threadElemCountMax
                        std::numeric_limits<TIdx>::max(),
                        // m_sharedMemSizeBytes
                        static_cast<size_t>(BlockSharedMemDynMember<>::staticAllocBytes()),
                        // m_globalMemSizeBytes
                        getMemBytes(dev)};
            }
        };

        template<typename TDim, typename TIdx>
        struct GetAccName<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            ALPAKA_FN_HOST static auto getAccName() -> std::string
            {
                return "AccCpuLibforkBlocks<" + std::to_string(TDim::value) + "D,"
                    + std::string(core::demangled<TIdx>) + ">";
            }
        };

        template<typename TDim, typename TIdx>
        struct TagToAcc<TagCpuLibforkBlocks, TDim, TIdx>
        {
            using type = AccCpuLibforkBlocks<TDim, TIdx>;
        };

        template<typename TDim, typename TIdx>
        struct AccToTag<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = TagCpuLibforkBlocks;
        };

        template<typename TDim, typename TIdx, typename TWorkDiv, typename TKernelFnObj, typename... TArgs>
        struct CreateTaskKernel<AccCpuLibforkBlocks<TDim, TIdx>, TWorkDiv, TKernelFnObj, TArgs...>
        {
            ALPAKA_FN_HOST static auto createTaskKernel(
                TWorkDiv const& workDiv,
                TKernelFnObj const& kernelFnObj,
                TArgs&&... args)
            {
                return TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>(
                    workDiv,
                    kernelFnObj,
                    std::forward<TArgs>(args)...);
            }
        };

        template<typename TDim, typename TIdx>
        struct DevType<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = DevCpu;
        };

        template<typename TDim, typename TIdx>
        struct DimType<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = TDim;
        };

        template<typename TDim, typename TIdx>
        struct IdxType<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = TIdx;
        };

        template<typename TDim, typename TIdx>
        struct PlatformType<AccCpuLibforkBlocks<TDim, TIdx>>
        {
            using type = PlatformCpu;
        };

        template<typename TDim, typename TIdx>
        struct IsSingleThreadAcc<AccCpuLibforkBlocks<TDim, TIdx>> : std::true_type
        {
        };

        template<typename TDim, typename TIdx>
        struct IsMultiThreadAcc<AccCpuLibforkBlocks<TDim, TIdx>> : std::false_type
        {
        };
    } // namespace trait
} // namespace alpaka

#endif // ALPAKA_ACC_CPU_B_LIBFORK_T_SEQ_ENABLED
