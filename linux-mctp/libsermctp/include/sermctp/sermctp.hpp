#pragma once
// Umbrella header for the public libsermctp API.
// Users should include this single header when linking against libsermctp.

#include "export.h"
#include "MctpFramer.hpp"
#include "LinuxMctpSerial.hpp"
#include "MctpNetlink.hpp"
#include "MctpBridge.hpp"

// Re-export primary namespace for convenience
namespace iotorch::sermctp {
    // nothing here; this header pulls in the public API types into the namespace
}

/* Example usage:
   #include <sermctp/sermctp.hpp>
   using namespace iotorch::sermctp;
   MctpFramer f; f.open("/dev/ttyS0");
*/
