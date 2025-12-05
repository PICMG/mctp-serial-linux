#/***
 * @file mctp-bridge.cpp
 * @brief Userspace tool to instantiate a serial-backed MCTP bridge.
 *
 * This program creates a persistent MCTP bridge device between the 
 * specified serial TTY and the Linux MCTP core via an mctpserial interface.
 *
 * Typical usage: run as root and point at a serial device (e.g. /dev/ttyUSB0).
 * The bridge persists the interface and forwards frames between the kernel
 * AF_MCTP endpoint and the serially-attached MCTP framer.
 *
 * Author: Doug Sandy <doug@picmg.org>
 * License: MIT No Attribution (MIT-0)
 *
 * Disclaimer: THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 * KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "MctpBridge.hpp"
#include <iostream>         // std::cout, std::cerr
#include <string>           // std::string, std::to_string
#include <cstdlib>          // std::system, atoi, EXIT_FAILURE / EXIT_SUCCESS
#include <atomic>           // std::atomic
#include <optional>         // std::optional, std::nullopt
#include <unordered_map>    // std::unordered_map
#include <algorithm>        // std::max, std::transform
#include <cctype>           // std::tolower
#include <chrono>           // std::chrono
#include <thread>           // std::this_thread::sleep_for
#include <csignal>          // std::signal
extern "C" {
    #include <getopt.h>
    #include <stdio.h>
    #include <unistd.h>
}

// options structure
struct CmdOptions {
    std::string tty;
    std::optional<BaudRate> baud;   // std::nullopt if not provided
    bool hwflow = false;            // default false
    std::optional<std::string> ifname;
    std::optional<int> local_eid;
    std::optional<int> remote_eid;
};  

/*
 * @brief Handle signals (e.g., SIGINT, SIGTERM) by setting the interrupted flag.
 *
 * @param signum  Signal number received.
 * @return void
 */
std::atomic<bool> interrupted{false};
void signalHandler(int signum) {
    std::cout << "\nCaught signal " << signum << ", cleaning up...\n";
    interrupted = true;
}

/**
 * @brief Maps a string like "B115200" to a BaudRate enum value.
 * @param str The baud rate string (e.g., "B9600", "B115200").
 * @return An optional BaudRate value if the string is valid; std::nullopt otherwise.
 */
std::optional<BaudRate> baudRateFromString(const std::string& str) {
    static const std::unordered_map<std::string, BaudRate> baudMap = {
        {"B0", BaudRate::BR_0},{"B50", BaudRate::BR_50},{"B75", BaudRate::BR_75},
        {"B110", BaudRate::BR_110},{"B134", BaudRate::BR_134},{"B150", BaudRate::BR_150},
        {"B200", BaudRate::BR_200},{"B300", BaudRate::BR_300},{"B600", BaudRate::BR_600},
        {"B1200", BaudRate::BR_1200},{"B1800", BaudRate::BR_1800},{"B2400", BaudRate::BR_2400},
        {"B4800", BaudRate::BR_4800},{"B9600", BaudRate::BR_9600},{"B19200", BaudRate::BR_19200},
        {"B38400", BaudRate::BR_38400},{"B57600", BaudRate::BR_57600},{"B115200", BaudRate::BR_115200},
        {"B230400", BaudRate::BR_230400}
    };

    auto it = baudMap.find(str);
    if (it != baudMap.end()) {
        return it->second;
    }
    return std::nullopt;
}

/**
 * @brief Print command-line usage for the program.
 *
 * @param progName  Program name (typically argv[0]) used in usage and examples.
 * @return void     Prints usage to stdout and returns.
 */
void printUsage(const char* progName) {
    std::cout << "Usage: " << progName << " --mode <BRIDGE|TEST> --tty <tty-path> [options]\n\n";

    std::cout << "Required:\n";
    std::cout << "  --tty  <tty-path>       Path to serial device (e.g. /dev/ttyS0, /dev/ttyUSB0).\n\n";
    std::cout << "  --remote-eid  <n>       Remote endpoint ID (1-254). \n";

    std::cout << "Optional:\n";
    std::cout << "  --baud <baud-string>    Baud rate string (e.g. B9600, B115200). If omitted, default B115200 is used\n";
    std::cout << "  --hwflow <TRUE|FALSE>   Hardware flow control. TRUE to enable RTS/CTS, FALSE (default) to disable.\n";
    std::cout << "  --ifname <name>         Name of MCTP interface for socket connections (e.g. mctp0). Default: use linux-assigned name.\n";
    std::cout << "  --local-eid <n>         Local endpoint ID (1-254). If not specified, the bridge is transparent.\n";
    std::cout << "  --help                  Show this help message and exit.\n\n";

    std::cout << "Examples:\n";
    std::cout << "  " << progName << " --tty /dev/ttyUSB0 --remote-eid 8 --baud B115200 --hwflow TRUE --ifname mctp0\n";
    std::cout << "  " << progName << " --tty /dev/ttyS1 --remote-eid 8\n\n";

    std::cout << "Notes:\n";
    std::cout << "  - The code is blocking and will run until iterrupted with SIGINT.\n";
    std::cout << std::flush;
}

/**
 * @brief Convert a string to lowercase.
 *
 * @param s  Input string to convert (modified copy).
 * @return std::string  Lowercased copy of the input.
 */
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return s;
}

/*
 * @brief Parse a boolean-like string into a boolean value.
 *
 * Accepts (case-insensitive): true/1/yes and false/0/no.
 *
 * @param s  Input string (e.g. "TRUE", "false", "1", "0").
 * @return std::optional<bool>  true/false on recognized values, std::nullopt otherwise.
 */
static std::optional<bool> parseBool(const std::string& s) {
    auto low = toLower(s);
    if (low == "true" || low == "1" || low == "yes") return true;
    if (low == "false" || low == "0" || low == "no") return false;
    return std::nullopt;
}

/**
 * @brief Parse and validate command-line arguments.
 *
 * Uses getopt_long to accept:
 *   --tty  <tty-path>     (required)
 *   --baud <baud-string>  (optional)
 *   --hwflow <TRUE|FALSE> (optional)
 *   --ifname <name>       (optional)
 *   --local-eid <n>       (optional)
 *   --remote-eid <n>      (optional)
 *   --help                (prints usage and returns std::nullopt)
 *
 * On parse/validation error this function prints usage (via printUsage)
 * and returns std::nullopt.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return std::optional<CmdOptions>  Validated options or std::nullopt on error.
 */
std::optional<CmdOptions> parseArgs(int argc, char** argv) {
    static struct option longOpts[] = {
        {"tty",     required_argument, nullptr, 't'},
        {"baud",    required_argument, nullptr, 'b'},
        {"hwflow",  required_argument, nullptr, 'f'},
        {"ifname",  required_argument, nullptr, 'i'},
        {"local-eid", required_argument, nullptr, 'L'},
        {"remote-eid",  required_argument, nullptr, 'R'},
        {"help",    no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    CmdOptions opts;
    bool seenMode = false;
    bool seenTty  = false;

    int opt;
    int longIndex = 0;
    while ((opt = getopt_long(argc, argv, "t:b:f:i:L:R:h", longOpts, &longIndex)) != -1) {
        switch (opt) {
        case 't':
            opts.tty = optarg;
            seenTty = true;
            break;
        case 'b': {
            auto b = baudRateFromString(optarg); // returns std::optional<BaudRate>
            if (!b) {
                std::cerr << "Invalid --baud value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.baud = *b;
            break;
        }
        case 'f': {
            auto pb = parseBool(optarg);
            if (!pb) {
                std::cerr << "Invalid --hwflow value: " << optarg << " (expect TRUE/FALSE)\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.hwflow = *pb;
            break;
        }
        case 'i':
            opts.ifname = std::string(optarg);
            break;
        case 'L': {
            int v = atoi(optarg);
            if (v < 1 || v > 254) {
                std::cerr << "Invalid --local-eid value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.local_eid = v;
            break;
        }
        case 'R': {
            int v = atoi(optarg);
            if (v < 1 || v > 254) {
                std::cerr << "Invalid --remote-eid value: " << optarg << "\n";
                printUsage(argv[0]);
                return std::nullopt;
            }
            opts.remote_eid = v;
            break;
        }
        case 'h':
        default:
            printUsage(argv[0]);
            return std::nullopt;
        }
    }

    // Validate required options
    if (!seenTty) {
        std::cerr << "Missing required option: --tty\n";
        printUsage(argv[0]);
        return std::nullopt;
    }
    if (!opts.remote_eid.has_value()) {
        std::cerr << "Missing required option: --remote-eid\n";
        printUsage(argv[0]);
        return std::nullopt;
    }
    return opts;
}

/**
 * @brief Program entry point: parse arguments, validate, and start bridge or test mode.
 *
 * Uses the long-option parser implemented in parseArgs().
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.
 * @return int EXIT_SUCCESS on success, EXIT_FAILURE on error.
 */
int main(int argc, char *argv[]) {
    auto maybeOpts = parseArgs(argc, argv);
    if (!maybeOpts) return EXIT_FAILURE; // parse error or --help

    CmdOptions opts = *maybeOpts;

    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Open the serial bridge and run until interrupted.
    MctpBridge mctpbridge;
    std::string ifname;
    if (opts.ifname) ifname = *opts.ifname;
    BaudRate baud = BaudRate::BR_115200;
    if (opts.baud) baud = *opts.baud;

    if (!mctpbridge.open(opts.tty.c_str(), baud, opts.hwflow, 
                opts.local_eid ? static_cast<uint8_t>(*opts.local_eid) : 0,{static_cast<uint8_t>(*opts.remote_eid)})) {
        std::cerr << "Failed to open serial bridge on " << opts.tty << "\n";
        return EXIT_FAILURE;
    }
    // show the bridge configuration
    std::cout << "MCTP Bridge running:\n";
    std::cout << "  TTY device: " << opts.tty << "\n";
    std::cout << "  Baud rate: ";
    if (opts.baud) {
        std::cout << static_cast<int>(*(opts.baud)) << "\n";
    } else {
        std::cout << "B115200 (default)\n";
    }
    std::cout << "  Hardware flow control: " << (opts.hwflow ? "Enabled" : "Disabled") << "\n";
    if (opts.ifname) {
        std::cout << "  MCTP interface name: " << *opts.ifname << "\n";
    } else {
        std::cout << "  MCTP interface name: " << mctpbridge.getMctpIfName() << "\n";
    }
    std::cout << "  Broadcast socket name: " << mctpbridge.getBroadcastName() << "\n";  
    std::cout << "  local EID: ";
    if (opts.local_eid) {
        std::cout << static_cast<int>(*opts.local_eid) << "\n";
    } else {
        std::cout << "Transparent mode (no local EID)\n";
    }
    std::cout << "  remote EID: " << static_cast<int>(*opts.remote_eid) << "\n"; 
    std::cout << "Press Ctrl-C to exit.\n";

    // Run until signalled
    while (!interrupted) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}
