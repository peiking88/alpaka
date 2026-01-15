/* Copyright 2025 Axel Huebl, Benjamin Worpitz, Erik Zenker, René Widera, Jan Stephan, Bernhard Manfred Gruber,
 *                Andrea Bocci
 * SPDX-License-Identifier: MPL-2.0
 */

#pragma once

// Specialized traits.
#include "alpaka/acc/Traits.hpp"
#include "alpaka/dev/Traits.hpp"
#include "alpaka/dim/Traits.hpp"
#include "alpaka/idx/Traits.hpp"
#include "alpaka/platform/Traits.hpp"

// Implementation details.
#include "alpaka/acc/AccCpuLibforkBlocks.hpp"
#include "alpaka/core/Config.hpp"
#include "alpaka/core/Decay.hpp"
#include "alpaka/core/ThreadPool.hpp"
#include "alpaka/dev/DevCpu.hpp"
#include "alpaka/idx/MapIdx.hpp"
#include "alpaka/kernel/KernelFunctionAttributes.hpp"
#include "alpaka/kernel/Traits.hpp"
#include "alpaka/meta/NdLoop.hpp"
#include "alpaka/platform/PlatformCpu.hpp"
#include "alpaka/workdiv/WorkDivMembers.hpp"

#include <algorithm>
#include <functional>
#include <future>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>

#if ALPAKA_DEBUG >= ALPAKA_DEBUG_MINIMAL
#    include <iostream>
#endif

#ifdef ALPAKA_ACC_CPU_B_LIBFORK_T_SEQ_ENABLED

namespace alpaka
{
    //! The CPU Libfork block accelerator execution task.
    template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
    class TaskKernelCpuLibforkBlocks final : public WorkDivMembers<TDim, TIdx>
    {
    private:
        // Use the existing ThreadPool for efficient thread management
        using ThreadPool = alpaka::core::detail::ThreadPool;

    public:
        template<typename TWorkDiv>
        ALPAKA_FN_HOST TaskKernelCpuLibforkBlocks(TWorkDiv&& workDiv, TKernelFnObj const& kernelFnObj, TArgs&&... args)
            : WorkDivMembers<TDim, TIdx>(std::forward<TWorkDiv>(workDiv))
            , m_kernelFnObj(kernelFnObj)
            , m_args(std::forward<TArgs>(args)...)
        {
            static_assert(
                Dim<std::decay_t<TWorkDiv>>::value == TDim::value,
                "The work division and the execution task have to be of the same dimensionality!");
        }

        //! Executes the kernel function object.
        ALPAKA_FN_HOST auto operator()() const -> void
        {
            ALPAKA_DEBUG_MINIMAL_LOG_SCOPE;

            std::apply([&](auto const&... args) { runWithArgs(args...); }, m_args);
        }

    private:
        ALPAKA_FN_HOST auto runWithArgs(std::decay_t<TArgs> const&... args) const -> void
        {
            auto const gridBlockExtent = getWorkDiv<Grid, Blocks>(*this);
            auto const blockThreadExtent = getWorkDiv<Block, Threads>(*this);
            auto const threadElemExtent = getWorkDiv<Thread, Elems>(*this);

            // Get the size of the block shared dynamic memory.
            auto const blockSharedMemDynSizeBytes = std::apply(
                [&](std::decay_t<TArgs> const&... args)
                {
                    return getBlockSharedMemDynSizeBytes<AccCpuLibforkBlocks<TDim, TIdx>>(
                        m_kernelFnObj,
                        blockThreadExtent,
                        threadElemExtent,
                        args...);
                },
                m_args);

#if ALPAKA_DEBUG >= ALPAKA_DEBUG_FULL
            std::cout << __func__ << " blockSharedMemDynSizeBytes: " << blockSharedMemDynSizeBytes << " B"
                      << std::endl;
#endif

            // The number of blocks in the grid.
            TIdx const numBlocksInGrid = gridBlockExtent.prod();

            // Optimize work division for CPU backends
            // If there are too many blocks with very small work per block, batch them together
            auto const hardwareConcurrency = std::thread::hardware_concurrency();
            constexpr TIdx minElementsPerTask = 4096; // Minimum work to amortize thread overhead
            constexpr TIdx maxTasks = 64; // Maximum number of tasks to queue

            // Calculate optimal chunking strategy
            TIdx blocksPerTask = 1;
            if(numBlocksInGrid > hardwareConcurrency * 4)
            {
                // We have too many small blocks - batch them together
                blocksPerTask = (numBlocksInGrid + maxTasks - 1) / maxTasks;
                // Ensure each task has at least minElementsPerTask elements
                TIdx elementsPerTask = blocksPerTask * blockThreadExtent.prod() * threadElemExtent.prod();
                if(elementsPerTask < minElementsPerTask)
                {
                    blocksPerTask = (minElementsPerTask + elementsPerTask - 1) / elementsPerTask;
                }
            }

            // Limit blocks per task to avoid excessive memory/cache pressure
            blocksPerTask = std::min(blocksPerTask, static_cast<TIdx>(numBlocksInGrid));

            TIdx const numTasks = (numBlocksInGrid + blocksPerTask - 1) / blocksPerTask;
            auto const threadPoolSize = std::min(
                static_cast<unsigned int>(hardwareConcurrency),
                static_cast<unsigned int>(numTasks));
            auto const finalThreadPoolSize = std::max(static_cast<unsigned int>(1), threadPoolSize);

#if ALPAKA_DEBUG >= ALPAKA_DEBUG_FULL
            std::cout << __func__ << " numBlocksInGrid: " << numBlocksInGrid
                      << " blocksPerTask: " << blocksPerTask
                      << " numTasks: " << numTasks
                      << " threadPoolSize: " << finalThreadPoolSize << std::endl;
#endif

            // Create thread pool with optimal size
            ThreadPool threadPool(finalThreadPoolSize);

            // Enqueue batched block tasks to the thread pool
            std::vector<std::future<void>> futures;
            futures.reserve(numTasks);

            for(TIdx taskIdx = 0; taskIdx < numTasks; ++taskIdx)
            {
                TIdx const startBlockIdx = taskIdx * blocksPerTask;
                TIdx const endBlockIdx = std::min(startBlockIdx + blocksPerTask, numBlocksInGrid);

                futures.emplace_back(threadPool.enqueueTask(
                    [this, startBlockIdx, endBlockIdx, gridBlockExtent, blockSharedMemDynSizeBytes, &args...]()
                    {
                        // Process a batch of blocks in this thread
                        for(TIdx blockIdx = startBlockIdx; blockIdx < endBlockIdx; ++blockIdx)
                        {
                            AccCpuLibforkBlocks<TDim, TIdx> acc(
                                *static_cast<WorkDivMembers<TDim, TIdx> const*>(this),
                                blockSharedMemDynSizeBytes);

                            acc.m_gridBlockIdx = mapIdx<TDim::value>(
                                Vec<DimInt<1u>, TIdx>(static_cast<TIdx>(blockIdx)),
                                gridBlockExtent);

                            std::apply(m_kernelFnObj, std::tuple_cat(std::tie(acc), m_args));

                            freeSharedVars(acc);
                        }
                    }));
            }

            // Wait for all tasks to complete
            for(auto& future : futures)
            {
                future.wait();
            }
        }

    private:
        TKernelFnObj m_kernelFnObj;
        std::tuple<std::decay_t<TArgs>...> m_args;
    };

    namespace trait
    {
        //! The CPU Libfork block execution task accelerator type trait specialization.
        template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
        struct AccType<TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>>
        {
            using type = AccCpuLibforkBlocks<TDim, TIdx>;
        };

        //! The CPU Libfork block execution task device type trait specialization.
        template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
        struct DevType<TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>>
        {
            using type = DevCpu;
        };

        //! The CPU Libfork block execution task dimension getter trait specialization.
        template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
        struct DimType<TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>>
        {
            using type = TDim;
        };

        //! The CPU Libfork block execution task platform type trait specialization.
        template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
        struct PlatformType<TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>>
        {
            using type = PlatformCpu;
        };

        //! The CPU Libfork block execution task idx type trait specialization.
        template<typename TDim, typename TIdx, typename TKernelFnObj, typename... TArgs>
        struct IdxType<TaskKernelCpuLibforkBlocks<TDim, TIdx, TKernelFnObj, TArgs...>>
        {
            using type = TIdx;
        };

        //! \brief Specialisation of class template FunctionAttributes
        //! \tparam TDev The device type.
        //! \tparam TDim The dimensionality of the accelerator device properties.
        //! \tparam TIdx The idx type of the accelerator device properties.
        //! \tparam TKernelFn Kernel function object type.
        //! \tparam TArgs Kernel function object argument types as a parameter pack.
        template<typename TDev, typename TDim, typename TIdx, typename TKernelFn, typename... TArgs>
        struct FunctionAttributes<AccCpuLibforkBlocks<TDim, TIdx>, TDev, TKernelFn, TArgs...>
        {
            //! \param dev The device instance
            //! \param kernelFn The kernel function object which should be executed.
            //! \param args The kernel invocation arguments.
            //! \return KernelFunctionAttributes instance. The default version always returns an instance with zero
            //! fields. For CPU, field of max threads allowed by kernel function for the block is 1.
            ALPAKA_FN_HOST static auto getFunctionAttributes(
                TDev const& dev,
                [[maybe_unused]] TKernelFn const& kernelFn,
                [[maybe_unused]] TArgs&&... args) -> alpaka::KernelFunctionAttributes
            {
                alpaka::KernelFunctionAttributes kernelFunctionAttributes;

                // set function properties for maxThreadsPerBlock to device properties, since API doesn't have function
                // properties function.
                auto const& props = alpaka::getAccDevProps<AccCpuLibforkBlocks<TDim, TIdx>>(dev);
                kernelFunctionAttributes.maxThreadsPerBlock = static_cast<int>(props.m_blockThreadCountMax);
                kernelFunctionAttributes.maxDynamicSharedSizeBytes
                    = static_cast<int>(alpaka::BlockSharedDynMemberAllocKiB * 1024);
                return kernelFunctionAttributes;
            }
        };
    } // namespace trait
} // namespace alpaka

#endif
