#ifndef PIRU_PAGING_TEST_HPP
#define PIRU_PAGING_TEST_HPP

#include <definitions.hpp>

namespace piru::tests {
    // Must run BEFORE paging::EnableShootdown().
    // Runs the stale-TLB test without shootdowns, to check that the test is able to detect stale TLB entries at all.
    void RunPagingControlTest();

    // Must run AFTER paging::EnableShootdown(). Returns true if every test passed.
    bool RunPagingTests();
}

#endif //PIRU_PAGING_TEST_HPP
