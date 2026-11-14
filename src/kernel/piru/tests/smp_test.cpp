#include "smp_test.hpp"

#include <atomic>

#include "kernel.hpp"
#include "logging.hpp"
#include "core/smp.hpp"
#include "boot/limine_definitions.hpp"

namespace {
    using namespace piru;
    using smp = core::smp;
    using Flags = memory::paging::Flags;

    constexpr uint64_t TimeoutMs        = 2000;
    constexpr size_t   FifoJobs         = 20;
    constexpr size_t   BurstJobs        = 32;    // == the smp ring buffer capacity
    constexpr size_t   WakeupIterations = 2000;
    constexpr size_t   ShootdownIters   = 50;    // per CPU
    // PML4 slot 384 like the paging tests, but a separate 1 GiB region.
    constexpr uint64_t ShootdownBase    = 0xFFFFC000C0000000ULL;

    // ---------------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------------
    size_t CpuCount() { return kernel::GetInstance().GetCpuCount(); }
    size_t CurrentCpuIndex() { return kernel::GetInstance().GetCpuIndex(interrupts::gdt::GetCpuId()); }
    uint32_t SelfId() { return interrupts::gdt::GetCpuId(); }

    // The n-th CPU that is not the calling CPU (n = 0 .. CpuCount()-2), as LAPIC id.
    uint32_t OtherCpu(size_t n) {
        auto& k = kernel::GetInstance();
        for (size_t i = 0; i < k.GetCpuCount(); i++) {
            auto id = static_cast<uint32_t>(k.GetCpuId(i));
            if (id == SelfId()) continue;
            if (n-- == 0) return id;
        }
        PANIC("[smp tests] OtherCpu index out of range");
    }

    // Spins (with interrupts enabled) until `value` reaches `target`, or the timeout expires.
    // A timeout means a job was never run, e.g. a lost wake-up in the idle loop.
    bool WaitFor(std::atomic<int>& value, int target, string_view test, uint32_t cpu) {
        auto& time = kernel::GetInstance().data.time;
        uint64_t start = time.GetMs();
        while (value.load() != target) {
            if (time.GetMs() - start > TimeoutMs) {
                LOG_ERROR("[smp:{}] TIMEOUT waiting for CPU {} ({} of the jobs never completed)", test, cpu, value.load() - target);
                return false;
            }
            asm volatile ("pause");
        }
        return true;
    }

    bool Report(string_view test, size_t failures) {
        if (failures == 0) LOG_INFO("[smp:{}] PASSED", test);
        else LOG_ERROR("[smp:{}] FAILED with {} failure(s)", test, failures);
        return failures == 0;
    }

    uint64_t XorShift(uint64_t& s) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return s;
    }

    // ---------------------------------------------------------------------------------
    // Test 1 + 2 + 8: CallOnAll runs exactly once per CPU, respects `self`, and jobs run with IF=1.
    // ---------------------------------------------------------------------------------
    struct once_ctx {
        std::atomic<int> hits[system::MaxCpuCount];
        std::atomic<int> interruptsOff{0};
    };
    once_ctx _once{};

    void OnceJob(void* arg) {
        auto* ctx = static_cast<once_ctx*>(arg);
        ctx->hits[CurrentCpuIndex()].fetch_add(1);
        if (!threading::spinlock::AreInterruptsEnabled())
            ctx->interruptsOff.fetch_add(1);
    }

    bool TestCallOnAll(bool self) {
        string_view name = self ? "all-self" : "all-noself";
        for (auto& h : _once.hits) h.store(0);
        _once.interruptsOff.store(0);

        smp::CallOnAll(OnceJob, &_once, /*wait*/ true, self);

        size_t failures = 0;
        const size_t me = CurrentCpuIndex();
        for (size_t i = 0; i < CpuCount(); i++) {
            int expected = (i == me && !self) ? 0 : 1;
            int got = _once.hits[i].load();
            if (got != expected) {
                LOG_ERROR("[smp:{}] CPU index {} ran the job {} time(s), expected {}", name, i, got, expected);
                failures++;
            }
        }

        // The calling CPU runs its own part with whatever IF it has (enabled), the APs must have IF=1 too.
        if (_once.interruptsOff.load() != 0) {
            LOG_ERROR("[smp:{}] {} job(s) ran with interrupts DISABLED (shootdowns would deadlock)", name, _once.interruptsOff.load());
            failures++;
        }

        return Report(name, failures);
    }

    // ---------------------------------------------------------------------------------
    // Test 3: jobs queued to one CPU run in FIFO order.
    // Test 4: exactly a full ring buffer worth of jobs, queued back to back.
    // ---------------------------------------------------------------------------------
    struct order_ctx {
        std::atomic<int> pos{0};
        int order[BurstJobs]{};
        std::atomic<int> remaining{0};
    };
    struct order_item {
        order_ctx* ctx;
        int index;
    };
    order_ctx _order{};
    order_item _orderItems[BurstJobs]{};

    void OrderJob(void* arg) {
        auto* item = static_cast<order_item*>(arg);
        int p = item->ctx->pos.fetch_add(1);
        if (p < static_cast<int>(BurstJobs))
            item->ctx->order[p] = item->index;
    }

    bool TestOrder(string_view name, size_t jobs) {
        const uint32_t target = OtherCpu(0);

        _order.pos.store(0);
        _order.remaining.store(static_cast<int>(jobs));
        for (size_t i = 0; i < jobs; i++) {
            _order.order[i] = -1;
            _orderItems[i] = { &_order, static_cast<int>(i) };
        }

        for (size_t i = 0; i < jobs; i++)
            smp::CallOn(target, OrderJob, &_orderItems[i], &_order.remaining);

        if (!WaitFor(_order.remaining, 0, name, target))
            return Report(name, 1);

        size_t failures = 0;
        if (_order.pos.load() != static_cast<int>(jobs)) {
            LOG_ERROR("[smp:{}] {} jobs ran, expected {}", name, _order.pos.load(), jobs);
            failures++;
        }
        for (size_t i = 0; i < jobs; i++) {
            if (_order.order[i] != static_cast<int>(i)) {
                LOG_ERROR("[smp:{}] position {} ran job {}, expected job {} (not FIFO)", name, i, _order.order[i], i);
                failures++;
                break;
            }
        }
        return Report(name, failures);
    }

    // ---------------------------------------------------------------------------------
    // Test 5: lost wake-up stress. One job at a time to a random AP, wait for each.
    // A gap between "queue is empty" and "hlt" in the idle loop makes this time out.
    // ---------------------------------------------------------------------------------
    std::atomic<int> _wakeupCounter{0};
    std::atomic<int> _wakeupRemaining{0};

    void WakeupJob(void*) {
        _wakeupCounter.fetch_add(1);
    }

    bool TestWakeup() {
        string_view name = "wakeup";
        uint64_t seed = 0x9E3779B97F4A7C15ULL;
        const size_t others = CpuCount() - 1;

        _wakeupCounter.store(0);
        for (size_t i = 0; i < WakeupIterations; i++) {
            uint32_t target = OtherCpu(XorShift(seed) % others);
            _wakeupRemaining.store(1);
            smp::CallOn(target, WakeupJob, nullptr, &_wakeupRemaining);
            if (!WaitFor(_wakeupRemaining, 0, name, target)) {
                LOG_ERROR("[smp:{}] got stuck at iteration {}", name, i);
                return Report(name, 1);
            }
        }

        size_t failures = 0;
        if (_wakeupCounter.load() != static_cast<int>(WakeupIterations)) {
            LOG_ERROR("[smp:{}] {} jobs ran, expected {}", name, _wakeupCounter.load(), WakeupIterations);
            failures++;
        }
        return Report(name, failures);
    }

    // ---------------------------------------------------------------------------------
    // Test 6: a job queues a job on another CPU. Requires that jobs run outside the queue lock.
    // ---------------------------------------------------------------------------------
    struct nested_ctx {
        uint32_t innerTarget{};
        std::atomic<int> remaining{0};
        std::atomic<int> innerRan{0};
        std::atomic<uint32_t> innerCpu{0};
    };
    nested_ctx _nested{};

    void NestedInnerJob(void* arg) {
        auto* ctx = static_cast<nested_ctx*>(arg);
        ctx->innerCpu.store(SelfId());
        ctx->innerRan.fetch_add(1);
    }

    void NestedOuterJob(void* arg) {
        auto* ctx = static_cast<nested_ctx*>(arg);
        smp::CallOn(ctx->innerTarget, NestedInnerJob, ctx, &ctx->remaining);
    }

    bool TestNested() {
        string_view name = "nested";
        if (CpuCount() < 3) {
            LOG_WARNING("[smp:{}] skipped, needs at least 3 CPUs", name);
            return true;
        }

        const uint32_t outer = OtherCpu(0);
        _nested.innerTarget = OtherCpu(1);
        _nested.remaining.store(2); // outer + inner
        _nested.innerRan.store(0);

        smp::CallOn(outer, NestedOuterJob, &_nested, &_nested.remaining);
        if (!WaitFor(_nested.remaining, 0, name, outer))
            return Report(name, 1);

        size_t failures = 0;
        if (_nested.innerRan.load() != 1) {
            LOG_ERROR("[smp:{}] inner job ran {} time(s), expected 1", name, _nested.innerRan.load());
            failures++;
        }
        if (_nested.innerCpu.load() != _nested.innerTarget) {
            LOG_ERROR("[smp:{}] inner job ran on CPU {}, expected CPU {}", name, _nested.innerCpu.load(), _nested.innerTarget);
            failures++;
        }
        return Report(name, failures);
    }

    // ---------------------------------------------------------------------------------
    // Test 7: every CPU maps/unmaps inside a job at the same time, so every job triggers
    // shootdowns that the other (job-running) CPUs must acknowledge. Hangs if jobs run with IF=0.
    // ---------------------------------------------------------------------------------
    std::atomic<int> _shootdownFailures{0};

    void ShootdownJob(void*) {
        auto& k = kernel::GetInstance();
        auto& paging = k.data.paging;
        auto& pmm = k.data.pmm;
        auto* state = k.GetCpu().pageState;
        const size_t cpu = CurrentCpuIndex();
        const uint64_t vaddr = ShootdownBase + cpu * system::PageSize;
        const uint64_t hhdm = boot::limine::hhdm_request.response->offset;

        for (size_t it = 0; it < ShootdownIters; it++) {
            auto phys = reinterpret_cast<uint64_t>(pmm.Allocate(1));
            if (!phys) { _shootdownFailures.fetch_add(1); return; }

            paging.MapPage(state, vaddr, phys, Flags::ReadWrite | Flags::NoExecute);

            const uint64_t pattern = (static_cast<uint64_t>(cpu) << 48) ^ it;
            *reinterpret_cast<volatile uint64_t*>(vaddr) = pattern;
            if (*reinterpret_cast<volatile uint64_t*>(phys + hhdm) != pattern)
                _shootdownFailures.fetch_add(1);

            paging.UnmapPage(state, vaddr); // shoots down: every other CPU must respond while in its job
            pmm.Free(reinterpret_cast<void*>(phys), 1);
        }
    }

    bool TestShootdownInJobs() {
        string_view name = "shootdown-in-jobs";
        auto& k = kernel::GetInstance();

        for (size_t i = 0; i < CpuCount(); i++) {
            if (k.data.paging.IsMapped(k.GetCpu().pageState, ShootdownBase + i * system::PageSize)) {
                LOG_ERROR("[smp:{}] test region already in use, skipping", name);
                return Report(name, 1);
            }
        }

        _shootdownFailures.store(0);
        smp::CallOnAll(ShootdownJob, nullptr, /*wait*/ true, /*self*/ true);
        return Report(name, static_cast<size_t>(_shootdownFailures.load()));
    }
}

namespace piru::tests {
    bool RunSmpTests() {
        if (!threading::spinlock::AreInterruptsEnabled()) {
            LOG_ERROR("[smp tests] caller has interrupts disabled, cannot wait on other CPUs");
            return false;
        }
        if (CpuCount() < 2) {
            LOG_WARNING("[smp tests] only 1 CPU, skipping");
            return true;
        }

        LOG_INFO("[smp tests] starting on {} CPUs", CpuCount());

        bool passed = true;
        passed &= TestCallOnAll(true);
        passed &= TestCallOnAll(false);
        passed &= TestOrder("fifo", FifoJobs);
        passed &= TestOrder("burst", BurstJobs);
        passed &= TestWakeup();
        passed &= TestNested();
        passed &= TestShootdownInJobs();

        if (passed) LOG_INFO("[smp tests] ALL PASSED");
        else LOG_ERROR("[smp tests] FAILED");
        return passed;
    }
}
