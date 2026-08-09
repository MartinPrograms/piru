#include "kernel.hpp"

#include <cpp/stream.hpp>
#include <cpp/fmt.hpp>
#include <logging.hpp>

#include "boot/limine_definitions.hpp"
#include "subsystems.hpp"

void Trampoline(limine_mp_info* info) {
    piru::kernel::GetInstance().CpuInit(info->lapic_id);
}

std::atomic<bool> _schedulerReady{false};

namespace piru {
    kernel & kernel::GetInstance() {
        static kernel kernel;
        return kernel;
    }

    void kernel::Bootstrap() {
        asm volatile ("cli");

        data.serial.Initialize();
        data.framebuffer.Initialize();
        data.pmm.Initialize();

        auto mp_response = boot::limine::mp_request.response;
        _bsp = mp_response->bsp_lapic_id;
        cpus[_bsp].tss.Initialize(_bsp, (void*)system::StackTop, (void*)system::DefaultFaultHandlerTop);
        cpus[_bsp].gdt.Initialize(_bsp, &cpus[_bsp].tss);

        data.idt.Initialize();
        data.idt.InitializeCpu();
        data.cpu.Initialize();
        data.cpu.InitializeCpu();
        data.paging.Initialize();
        data.paging.SwitchToKernelPageTable(); // Switch to shared higher half
        data.acpi.Initialize();

        cpus[_bsp].apic.Initialize(data.paging, data.acpi);
        data.ioapic_manager.Initialize();

        data.time.Calibrate(cpus[_bsp].apic, data.acpi);

        _cpuCount = mp_response->cpu_count;
        for (size_t i = 0; i < _cpuCount; i++) {
            _cpuIds[i] = mp_response->cpus[i]->lapic_id;
        }

        // This core is now fully initialized, and we can call CpuInit with each other core.
        for (size_t i = 0; i < mp_response->cpu_count; i++) {
            auto core = mp_response->cpus[i];
            if (core->lapic_id == _bsp) continue; // Skip BSP

            auto stackPageCount = ((uintptr_t)system::StackTop - (uintptr_t)system::StackBottom) / system::PageSize;
            auto physRsp0 = data.pmm.Allocate(stackPageCount);
            auto virtRsp0 = data.paging.FindAvailableVirtualAddressRangeKernel(stackPageCount);
            data.paging.MapRange(GetCpu().pageState, virtRsp0.start, (uintptr_t)physRsp0, stackPageCount, memory::paging::Flags::ReadWrite);

            auto faultStackPageCount = ((uintptr_t)system::DefaultFaultHandlerTop - (uintptr_t)system::DefaultFaultHandlerBottom) / system::PageSize;
            auto physIst1 = data.pmm.Allocate(faultStackPageCount);
            auto virtIst1 = data.paging.FindAvailableVirtualAddressRangeKernel(faultStackPageCount);
            data.paging.MapRange(GetCpu().pageState, virtIst1.start, (uintptr_t)physIst1, faultStackPageCount, memory::paging::Flags::ReadWrite);

            cpus[core->lapic_id].stackBase = virtRsp0.end;
            cpus[core->lapic_id].faultHandlerStackBase = virtIst1.end;

            auto trampoline = Trampoline;
            __atomic_store(&core->goto_address, &trampoline, __ATOMIC_RELEASE);
        }

        CpuInit(_bsp);

        while (_initializedCount < _cpuCount) {
            asm ("pause"); // park
        }

        LOG_INFO("All {} CPUs initialized successfully", _cpuCount);
    }

    // Runs per core
    void kernel::CpuInit(uint32_t cpuId) {
        asm volatile ("cli");

        LOG_DEBUG("Initializing CPU with ID: {}", cpuId);

        if (cpuId != _bsp) {
            cpus[cpuId].tss.Initialize(cpuId, (void*)cpus[cpuId].stackBase, (void*)cpus[cpuId].faultHandlerStackBase);
            cpus[cpuId].gdt.Initialize(cpuId, &cpus[cpuId].tss);
            data.idt.InitializeCpu();
            data.cpu.InitializeCpu();
            data.paging.SwitchToKernelPageTable();
            cpus[cpuId].apic.Initialize(data.paging, data.acpi);
        }

        LOG_DEBUG("CPU with ID: {} initialized successfully", cpuId);

        auto flags = _initLock.lock(false);
        _initializedCount++;
        _initLock.unlock(flags);

        if (cpuId != _bsp) {
            while (_schedulerReady.load(std::memory_order_acquire) == false) {
                asm ("pause");
            }

            auto threading = data.subsystem_manager.GetSubsystem<subsystems::thread_scheduler>();
            threading->StartCore();
        }
    }

    void kernel::InitializeSubsystems() {
        data.subsystem_manager.Initialize();
    }

    struct MandelbrotJob {
        int width = 400;
        int height = 400;
        int maxIterations = 25;
        uint32_t* framebuffer = nullptr;

        double minRe = -2.0;
        double maxRe = 1.0;
        double minIm = -1.2;
        double maxIm = 1.2;

        std::atomic<int> nextRow{0};
    };

    void RenderMandelbrotWorker(MandelbrotJob* job) {
        double reFactor = (job->maxRe - job->minRe) / (job->width - 1);
        double imFactor = (job->maxIm - job->minIm) / (job->height - 1);
        double aspect = (double)job->width / (double)job->height;

        while (true) {
            // Fetch the next available row atomically
            int y = job->nextRow.fetch_add(1, std::memory_order_relaxed);
            if (y >= job->height) {
                break; // All rows are being processed or finished
            }

            // Calculate the imaginary component for this specific row
            double c_im = job->maxIm - y * imFactor;
            uint32_t* rowBuffer = &job->framebuffer[y * job->width];

            for (int x = 0; x < job->width; ++x) {
                double c_re = job->minRe + (x * aspect) * reFactor;

                double z_re = c_re;
                double z_im = c_im;
                int isInside = 1;
                int n = 0;

                for (n = 0; n < job->maxIterations; ++n) {
                    double z_re2 = z_re * z_re;
                    double z_im2 = z_im * z_im;

                    if (z_re2 + z_im2 > 4.0) {
                        isInside = 0;
                        break;
                    }

                    z_im = 2.0 * z_re * z_im + c_im;
                    z_re = z_re2 - z_im2 + c_re;
                }

                // Map iteration counts to a visible color scheme
                if (isInside) {
                    rowBuffer[x] = 0x00000000; // Black for the set interior
                } else {
                    // A smooth, fast procedural blue/cyan gradient
                    uint8_t r = 0;
                    uint8_t g = (n * 7) % 256;
                    uint8_t b = (n * 13) % 256;

                    // Assuming XRGB or ARGB 32-bit color format
                    rowBuffer[x] = (r << 16) | (g << 8) | b;
                }
            }
        }
    }

    void DisplayFramebuffer(MandelbrotJob* job, limine_framebuffer_response *framebuffer) {
        for (int y = 0; y < job->height; ++y) {
            for (int x = 0; x < job->width; ++x) {
                uint32_t color = job->framebuffer[y * job->width + x];
                ((uint32_t*)framebuffer->framebuffers[0]->address)[y * framebuffer->framebuffers[0]->pitch / 4 + x] = color;
            }
        }
    }

    void kernel::Start() {
        data.paging.EnableShootdown();

        auto threading = data.subsystem_manager.GetSubsystem<subsystems::thread_scheduler>();

        // FIX: Allocate the job on the heap so it survives function exit!
        auto* job = new MandelbrotJob();
        job->framebuffer = new uint32_t[job->width * job->height];

        RenderMandelbrotWorker(job); // Pass pointer
        DisplayFramebuffer(job, boot::limine::framebuffer_request.response);

        LOG_DEBUG("The above took quite a bit. Since it was singlethreaded!");
        LOG_DEBUG("Now we are starting the multithreaded one!");
        memset(job->framebuffer, 0, job->width * job->height * sizeof(uint32_t));

        asm volatile ("sti");
        GetCpu().apic.Sleep(1000);
        DisplayFramebuffer(job, boot::limine::framebuffer_request.response);

        job->nextRow.store(0, std::memory_order_relaxed);

        // Capture the pointer by value
        threading->CreateThread([](void* arg) {
            auto* localJob = (MandelbrotJob*)arg;
            while (true) {
                if (localJob->nextRow.load(std::memory_order_relaxed) >= localJob->height) {
                    DisplayFramebuffer(localJob, boot::limine::framebuffer_request.response);
                    break;
                }
                GetInstance().data.subsystem_manager.GetSubsystem<subsystems::thread_scheduler>()->Yield();
            }
            LOG_DEBUG("Finished multithreaded mandelbrot");
            // Optional: delete localJob->framebuffer; delete localJob; if you want to clean up here
        }, job, false, GetCpu().pageState);

        for (size_t i = 0; i < GetCpuCount(); i++) {
            threading->CreateThread([](void* arg) {
                RenderMandelbrotWorker((MandelbrotJob*)arg);
            }, job, false, GetCpu().pageState);
        }

        _schedulerReady.store(true, std::memory_order_release);
        threading->StartCore();
    }

    struct kernel::cpu &kernel::GetCpu() {
        auto id = interrupts::gdt::GetCpuId();
        return cpus[id];
    }

    namespace core {
        [[noreturn]] void Panic(const char* message) {
            asm volatile ("cli");
            auto &kernel = kernel::GetInstance();
            kernel.GetCpu().apic.SendIPIAllExcludingSelf(interrupts::idt::PANIC_VECTOR);

            // Halt the CPU
            while (true) {
                asm volatile("hlt");
            }
        }

        extern "C" [[noreturn]] void abort() {
            Panic("Kernel abort() called");
        }

        threading::spinlock & GetLogLock() {
            static threading::spinlock logLock;
            return logLock;
        }
    }
} // piru