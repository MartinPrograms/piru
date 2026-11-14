#include "paging_test.hpp"

#include <atomic>

#include "kernel.hpp"
#include "logging.hpp"
#include "boot/limine_definitions.hpp"
#include "core/smp.hpp"

namespace {
    using namespace piru;
    using Flags = memory::paging::Flags;

    // Virtual regions used only by these tests. PML4 slot 384, far away from the HHDM (slot 256)
    // and the kernel image / FindAvailableVirtualAddressRangeKernel range (slot 511).
    constexpr uint64_t StressBase      = 0xFFFFC00000000000ULL; // 2 MiB aligned -> all slots share one PT
    constexpr uint64_t ControlTlbPage  = 0xFFFFC00040000000ULL; // separate 1 GiB region
    constexpr uint64_t TlbPage         = 0xFFFFC00080000000ULL; // separate 1 GiB region

    constexpr size_t StressIterations  = 500;  // per CPU
    constexpr size_t StressSlotsPerCpu = 64;   // 64 * 8 CPUs = 512 slots = exactly one PT
    constexpr size_t TlbRounds         = 50;
    constexpr size_t ControlTlbRounds  = 10;
    constexpr size_t TlbWarmReads      = 200;
    constexpr size_t MaxLoggedFailures = 8;

    constexpr uint64_t ValueA = 0xAAAAAAAAAAAAAAAAULL;
    constexpr uint64_t ValueB = 0xBBBBBBBBBBBBBBBBULL;

    // Jobs run on every CPU through smp::CallOnAll (APs from their idle loop, the calling CPU directly).
    size_t CurrentCpuIndex() {
        return kernel::GetInstance().GetCpuIndex(interrupts::gdt::GetCpuId());
    }

    bool IsBsp() {
        return interrupts::gdt::GetCpuId() == kernel::GetInstance().GetBsp();
    }

    void RunOnAllCpus(core::smp::CallFunction fn, void* arg) {
        core::smp::CallOnAll(fn, arg, /*wait*/ true, /*self*/ true);
    }

    // ---------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------
    uint64_t Hhdm() {
        return boot::limine::hhdm_request.response->offset;
    }

    void Fill(uint64_t phys, uint64_t value) {
        auto p = reinterpret_cast<volatile uint64_t*>(phys + Hhdm());
        for (size_t i = 0; i < system::PageSize / sizeof(uint64_t); i++)
            p[i] = value;
    }

    uint64_t ReadVirt(uint64_t vaddr) {
        return *reinterpret_cast<volatile uint64_t*>(vaddr);
    }

    // ---------------------------------------------------------------------------------
    // Test 1: SMP map / write / verify / unmap stress.
    // Every CPU uses its own slots, but the slots are interleaved (slot = k * cpuCount + cpu),
    // so all CPUs constantly create, use and free the SAME page table (and its PD).
    // ---------------------------------------------------------------------------------
    struct stress_ctx {
        std::atomic<size_t> failures{0};
    };

    void StressJob(void* arg) {
        const size_t cpu = CurrentCpuIndex();
        auto* ctx = static_cast<stress_ctx*>(arg);
        auto& k = kernel::GetInstance();
        auto& paging = k.data.paging;
        auto& pmm = k.data.pmm;
        auto* state = k.GetCpu().pageState;
        const size_t n = k.GetCpuCount();

        auto fail = [&](const char* what, uint64_t vaddr, uint64_t expected, uint64_t got) {
            auto count = ctx->failures.fetch_add(1);
            if (count < MaxLoggedFailures)
                LOG_ERROR("[stress] CPU {}: {} at {} (expected {}, got {})", cpu, what, (void*)vaddr, (void*)expected, (void*)got);
        };

        for (size_t it = 0; it < StressIterations; it++) {
            const size_t slot = (it % StressSlotsPerCpu) * n + cpu;
            const uint64_t vaddr = StressBase + slot * system::PageSize;

            auto phys = reinterpret_cast<uint64_t>(pmm.Allocate(1));
            if (!phys) {
                fail("pmm.Allocate returned null", vaddr, 1, 0);
                return;
            }

            paging.MapPage(state, vaddr, phys, Flags::ReadWrite | Flags::NoExecute);

            auto translated = paging.GetPhysicalAddress(state, vaddr);
            if (translated != phys)
                fail("GetPhysicalAddress mismatch", vaddr, phys, translated);

            // Write through the new mapping, read it back through the mapping AND through the HHDM.
            const uint64_t pattern = (static_cast<uint64_t>(cpu) << 56) ^ (static_cast<uint64_t>(it) << 20) ^ slot;
            auto v = reinterpret_cast<volatile uint64_t*>(vaddr);
            auto h = reinterpret_cast<volatile uint64_t*>(phys + Hhdm());

            for (size_t i = 0; i < system::PageSize / sizeof(uint64_t); i++)
                v[i] = pattern + i;

            for (size_t i = 0; i < system::PageSize / sizeof(uint64_t); i++) {
                if (v[i] != pattern + i) { fail("readback via mapping", vaddr + i * 8, pattern + i, v[i]); break; }
                if (h[i] != pattern + i) { fail("readback via HHDM (wrong physical page?)", vaddr + i * 8, pattern + i, h[i]); break; }
            }

            paging.UnmapPage(state, vaddr);

            if (paging.IsMapped(state, vaddr))
                fail("still mapped after UnmapPage", vaddr, 0, paging.GetPhysicalAddress(state, vaddr));

            pmm.Free(reinterpret_cast<void*>(phys), 1);
        }
    }

    bool RunStressTest() {
        auto& k = kernel::GetInstance();
        auto* state = k.GetCpu().pageState;
        const size_t n = k.GetCpuCount();
        const size_t totalSlots = StressSlotsPerCpu * n;

        LOG_INFO("[stress] {} CPUs x {} iterations, {} interleaved slots at {}", n, StressIterations, totalSlots, (void*)StressBase);

        stress_ctx ctx{};
        RunOnAllCpus(StressJob, &ctx);

        // Afterwards nothing in the region may be mapped any more.
        for (size_t slot = 0; slot < totalSlots; slot++) {
            uint64_t vaddr = StressBase + slot * system::PageSize;
            if (k.data.paging.IsMapped(state, vaddr)) {
                LOG_ERROR("[stress] slot {} ({}) still mapped after the test", slot, (void*)vaddr);
                ctx.failures.fetch_add(1);
            }
        }

        auto failures = ctx.failures.load();
        if (failures == 0) LOG_INFO("[stress] PASSED");
        else LOG_ERROR("[stress] FAILED with {} failure(s)", failures);
        return failures == 0;
    }

    // ---------------------------------------------------------------------------------
    // Test 2: stale TLB detection.
    // The BSP maps V -> A. Every AP reads V a lot (so V -> A ends up in its TLB) and reports "warmed".
    // The BSP then unmaps V and maps V -> B (different physical page, different contents), and
    // publishes the round number. Every AP then reads V once: if it still sees A's contents, its
    // TLB was stale, i.e. the shootdown did not reach / did not wait for that CPU.
    // ---------------------------------------------------------------------------------
    struct tlb_ctx {
        uint64_t vaddr{};
        uint64_t physA{};
        uint64_t physB{};
        size_t rounds{};
        bool logStale{};
        std::atomic<size_t> warmed{0};
        std::atomic<size_t> round{0};
        std::atomic<size_t> reported{0};
        std::atomic<size_t> stale{0};
    };

    uint64_t ExpectedValueForRound(size_t round) {
        return (round % 2 == 1) ? ValueB : ValueA; // round 0 (initial mapping) = A
    }

    void TlbJob(void* arg) {
        const size_t cpu = CurrentCpuIndex();
        const bool isBsp = IsBsp();
        auto* ctx = static_cast<tlb_ctx*>(arg);
        auto& k = kernel::GetInstance();
        auto& paging = k.data.paging;
        auto* state = k.GetCpu().pageState;
        const size_t aps = k.GetCpuCount() - 1;

        for (size_t r = 1; r <= ctx->rounds; r++) {
            if (isBsp) {
                // Wait until every AP has the current mapping cached.
                while (ctx->warmed.load(std::memory_order_acquire) < aps * r)
                    asm volatile ("pause");

                uint64_t newPhys = (ExpectedValueForRound(r) == ValueB) ? ctx->physB : ctx->physA;
                paging.UnmapPage(state, ctx->vaddr);   // with shootdown enabled: returns only after every CPU flushed
                paging.MapPage(state, ctx->vaddr, newPhys, Flags::ReadWrite | Flags::NoExecute);

                ctx->round.store(r, std::memory_order_release);

                while (ctx->reported.load(std::memory_order_acquire) < aps * r)
                    asm volatile ("pause");
            } else {
                uint64_t sink = 0;
                for (size_t i = 0; i < TlbWarmReads; i++)
                    sink += ReadVirt(ctx->vaddr);
                (void)sink;

                ctx->warmed.fetch_add(1, std::memory_order_release);

                while (ctx->round.load(std::memory_order_acquire) < r)
                    asm volatile ("pause");

                uint64_t got = ReadVirt(ctx->vaddr);
                uint64_t expected = ExpectedValueForRound(r);
                if (got != expected) {
                    auto count = ctx->stale.fetch_add(1);
                    if (ctx->logStale && count < MaxLoggedFailures)
                        LOG_ERROR("[tlb] CPU {} round {}: STALE read at {} (expected {}, got {})", cpu, r, (void*)ctx->vaddr, (void*)expected, (void*)got);
                }

                ctx->reported.fetch_add(1, std::memory_order_release);
            }
        }
    }

    size_t RunTlbTest(uint64_t vaddr, size_t rounds, bool logStale) {
        auto& k = kernel::GetInstance();
        auto& paging = k.data.paging;
        auto& pmm = k.data.pmm;
        auto* state = k.GetCpu().pageState;

        tlb_ctx ctx{};
        ctx.vaddr = vaddr;
        ctx.rounds = rounds;
        ctx.logStale = logStale;
        ctx.physA = reinterpret_cast<uint64_t>(pmm.Allocate(1));
        ctx.physB = reinterpret_cast<uint64_t>(pmm.Allocate(1));
        if (!ctx.physA || !ctx.physB)
            PANIC("[tlb] failed to allocate test pages");

        Fill(ctx.physA, ValueA);
        Fill(ctx.physB, ValueB);

        paging.MapPage(state, vaddr, ctx.physA, Flags::ReadWrite | Flags::NoExecute);

        RunOnAllCpus(TlbJob, &ctx);

        paging.UnmapPage(state, vaddr);
        pmm.Free(reinterpret_cast<void*>(ctx.physA), 1);
        pmm.Free(reinterpret_cast<void*>(ctx.physB), 1);

        return ctx.stale.load();
    }

    bool CheckRegionsUnused() {
        auto& k = kernel::GetInstance();
        auto* state = k.GetCpu().pageState;
        const uint64_t probes[] = { StressBase, ControlTlbPage, TlbPage };
        for (auto p : probes) {
            if (k.data.paging.IsMapped(state, p)) {
                LOG_ERROR("[paging tests] test region {} is already in use, not running tests", (void*)p);
                return false;
            }
        }
        return true;
    }

    bool CheckPreconditions() {
        if (!threading::spinlock::AreInterruptsEnabled()) {
            LOG_ERROR("[paging tests] BSP has interrupts disabled, shootdown IPIs could never be acknowledged");
            return false;
        }
        if (kernel::GetInstance().GetCpuCount() < 2) {
            LOG_WARNING("[paging tests] only 1 CPU, SMP tests are meaningless");
            return false;
        }
        return CheckRegionsUnused();
    }
}

namespace piru::tests {
    void RunPagingControlTest() {
        if (!CheckPreconditions()) return;

        LOG_INFO("[tlb-control] running {} rounds WITHOUT shootdown (stale reads expected)", ControlTlbRounds);
        auto stale = RunTlbTest(ControlTlbPage, ControlTlbRounds, false);
        auto total = ControlTlbRounds * (kernel::GetInstance().GetCpuCount() - 1);

        if (stale > 0)
            LOG_INFO("[tlb-control] {}/{} stale reads observed -> the TLB test can detect a missing shootdown", stale, total);
        else
            LOG_WARNING("[tlb-control] 0/{} stale reads: this machine/emulator did not keep stale entries, so the TLB test cannot prove much here", total);
    }

    bool RunPagingTests() {
        if (!CheckPreconditions()) return false;

        LOG_INFO("[paging tests] starting");

        LOG_INFO("[tlb] running {} rounds WITH shootdown (0 stale reads expected)", TlbRounds);
        auto stale = RunTlbTest(TlbPage, TlbRounds, true);
        bool tlbPassed = stale == 0;
        if (tlbPassed) LOG_INFO("[tlb] PASSED");
        else LOG_ERROR("[tlb] FAILED: {} stale read(s)", stale);

        bool stressPassed = RunStressTest();

        bool passed = tlbPassed && stressPassed;
        if (passed) LOG_INFO("[paging tests] ALL PASSED");
        else LOG_ERROR("[paging tests] FAILED");
        return passed;
    }
}
