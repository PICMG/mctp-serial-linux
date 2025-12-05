#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "BcastMessenger.hpp"
#include "sermctp/detail/MctpFramer_impl.hpp"
#include "sermctp/detail/MctpBridge_impl.hpp"
#include "MctpNetlink_impl.hpp"
#include <functional>
#include "Log.hpp"
#include <cstdlib>
#include <cstring>

// C linkage for tests implemented in C++ files that export C symbols

// Simple test harness
using TestFn = std::function<bool()>;

struct TestCase { const char* name; TestFn fn; };

static bool run_test(const TestCase &t) {
    std::cout << "[ RUN ] " << t.name << "\n";
    try {
        bool ok = t.fn();
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name << "\n";
        return ok;
    } catch (const std::exception &e) {
        std::cout << "[ EXC ] " << t.name << " : " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cout << "[ EXC UNKNOWN ] " << t.name << "\n";
        return false;
    }
}

int main() {
    // Enable extra verbosity when MCTP_VERBOSE=1 (useful to debug flaky I/O)
    const char *mv = std::getenv("MCTP_VERBOSE");
    if (mv && (mv[0] == '1' || strcmp(mv, "true") == 0 || strcmp(mv, "TRUE") == 0)) {
        util::setVerbose(true);
    }
    std::vector<TestCase> tests;

    // (ReceiveBuffer tests removed)

    // BcastMessenger: try setup on a temp socket path (safe)
    tests.push_back({"BcastMessenger_setup", [](){
        BcastMessenger b;
        std::string path = "/tmp/test-bcast-" + std::to_string(::getpid()) + ".sock";
        bool ok = b.open(path);
        // Do not call acceptConnection() here (would block). Treat setup success as pass.
        return ok;
    }});

    // BcastMessenger transfer test (bidirectional messaging)
    extern bool BcastMessenger_transfer();
    tests.push_back({"BcastMessenger_transfer", [](){ return BcastMessenger_transfer(); }});

    // MctpFramer: basic constructor and rxEmpty
    tests.push_back({"MctpFramer_basic", [](){
        MctpFramer s;
        return s.rxEmpty();
    }});

    // Full framer decode tests using serialized frames
    extern bool MctpFramer_decode_test();
    tests.push_back({"MctpFramer_decode", [](){ return MctpFramer_decode_test(); }});

    // (ReceiveBuffer tests removed)

    // (NullModem tests removed)

    // MctpNetlink: call a best-effort helper and consider success even if it returns false
    // (Netlink requires privileges; we only ensure the call doesn't crash).
    tests.push_back({"MctpNetlink_no_crash", [](){
        bool a = mctpnet::setMctpInterfaceName("none","none");
        // Accept either true or false as long as it returned (no crash)
        (void)a;
        return true;
    }});

    

    // MctpBridge: construct/destruct only
    tests.push_back({"MctpBridge_construct", [](){
        MctpBridge *b = nullptr;
        try { b = new MctpBridge(); } catch(...) { return false; }
        delete b;
        return true;
    }});

    // Bridge stress test (integration): runs interactive stress test when
    // executed as root; otherwise the test will indicate skip and return true
    // so unit test runs are not broken.
    extern bool bridge_stress_test();
    tests.push_back({"bridge_stress", [](){ return bridge_stress_test(); }});

    // Linux serial integration test: requires root and real devices; expose
    // as part of the aggregated runner by providing a function in the
    // test source that returns `bool`.
    extern bool linuxserial_test();
    tests.push_back({"linuxserial", [](){ return linuxserial_test(); }});

    // Run tests
    int passed = 0;
    for (const auto &t : tests) {
        if (run_test(t)) ++passed;
    }

    std::cout << "\nSummary: " << passed << " / " << tests.size() << " tests passed.\n";
    return (passed == (int)tests.size()) ? 0 : 1;
}
