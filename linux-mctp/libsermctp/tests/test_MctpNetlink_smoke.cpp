// Smoke tests for MctpNetlink public helpers. These tests do not modify
// production sources; they exercise the public API to ensure calls return
// (no crash). They do not require special privileges and accept either
// success or failure from the helpers as long as the call completes.

#include "MctpNetlink_impl.hpp"
#include <string>

bool MctpNetlink_smoke_test()
{
    bool ok = true;

    // Call each public helper with safe inputs where possible. We treat
    // either true or false as acceptable; the goal is to ensure no crash
    // or undefined behaviour when invoked from userland tests.
    try {
        // Use interface name unlikely to exist; functions should return
        // false but must not crash.
        (void)mctpnet::setMctpInterfaceName("nonexistent_iface", "newname");
        (void)mctpnet::setMctpLocalEid("nonexistent_iface", 42);
        (void)mctpnet::addMctpRoute("nonexistent_iface", 5);
        (void)mctpnet::removeMctpRoute("nonexistent_iface", 5);
        (void)mctpnet::setMctpInterfaceStatus("nonexistent_iface", true);
    } catch (...) {
        ok = false;
    }

    return ok;
}
