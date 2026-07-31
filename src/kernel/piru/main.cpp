#include <definitions.hpp>

#include "kernel.hpp"
#include "logging.hpp"

extern "C" void main() {
    auto &kernel = piru::kernel::GetInstance();
    kernel.Bootstrap();
    kernel.InitializeSubsystems();
    kernel.Start();
    PANIC("Kernel main returned unexpectedly");
}
