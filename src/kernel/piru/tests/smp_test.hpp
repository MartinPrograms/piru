#ifndef PIRU_SMP_TEST_HPP
#define PIRU_SMP_TEST_HPP

#include <definitions.hpp>

namespace piru::tests {
    // Tests core::smp (CallOn / CallOnAll).
    // Must run on the BSP with interrupts enabled, while all APs are idling.
    // Must run AFTER paging::EnableShootdown() (one test performs shootdowns inside jobs).
    // Returns true if every test passed.
    bool RunSmpTests();
}

#endif //PIRU_SMP_TEST_HPP
