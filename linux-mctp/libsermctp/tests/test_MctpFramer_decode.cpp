/**
 * @file test_MctpFramer_decode.cpp
 * @brief Tests that MctpFramer correctly decodes serial-encoded frames.
 *
 * This test reads `tests/mctp_random_messages.json`, extracts the `raw_frame`
 * entries and writes the serialized bytes into a pipe which is attached to a
 * `MctpFramer` via `openFd()`. The test waits for the framer to signal a
 * received packet and then verifies the decoded packet matches the expected
 * device-independent payload (raw_frame minus serial header and serial FCS).
 */

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <sys/select.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include "sermctp/detail/MctpFramer_impl.hpp"
#include "Log.hpp"
#include "mctp_random_messages_embedded.h"
#include <limits.h>
#include <unistd.h>

static bool write_all_fd(int fd, const std::vector<uint8_t> &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t w = ::write(fd, data.data() + sent, data.size() - sent);
        if (w > 0) { sent += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// Frame spec includes optional bad_type metadata extracted from the JSON so
// tests can exercise corrupt/partial cases.
struct FrameSpec { std::vector<uint8_t> raw; std::string bad_type; };

// Parse the JSON file for "raw_frame" string values and optional "bad_type"
// metadata, returning a list of FrameSpec entries.
static std::vector<FrameSpec> parse_raw_frames(const std::string &frames_path) {
    std::vector<FrameSpec> out;
    std::string s;
    bool loaded = false;
    // Try the provided path first (relative to the current working directory)
    {
        std::ifstream f(frames_path);
        if (f) {
            s.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            loaded = true;
        }
    }

    // If not found, try locating the test executable and look for the JSON next to it
    if (!loaded) {
        char exe_path[PATH_MAX] = {0};
        ssize_t r = ::readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
        if (r > 0) {
            exe_path[r] = '\0';
            std::string exe_s(exe_path);
            size_t p = exe_s.find_last_of('/');
            if (p != std::string::npos) {
                std::string candidate = exe_s.substr(0, p) + "/mctp_random_messages.json";
                std::ifstream f2(candidate);
                if (f2) {
                    s.assign((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
                    loaded = true;
                }
            }
        }
    }

    // If still not loaded from disk, fall back to an embedded JSON fixture
    if (!loaded) {
        std::string embedded = get_embedded_mctp_random_messages_json();
        if (!embedded.empty()) {
            s = embedded;
            loaded = true;
        }
    }

    if (!loaded) return out;

    size_t pos = 0;
    while (true) {
        pos = s.find("\"raw_frame\"", pos);
        if (pos == std::string::npos) break;
        // find colon after the key, then the opening quote of the string value
        size_t colon = s.find(':', pos);
        if (colon == std::string::npos) break;
        size_t openq = s.find('"', colon+1);
        if (openq == std::string::npos) break;
        size_t start = openq + 1;
        size_t end = s.find('"', start);
        if (end == std::string::npos) break;
        std::string val = s.substr(start, end - start);
        // split by spaces into bytes
        std::istringstream iss(val);
        std::string tok;
        std::vector<uint8_t> bytes;
        while (iss >> tok) {
            if (tok.size() >= 2 && tok[0]=='0' && (tok[1]=='x' || tok[1]=='X')) {
                if (tok.back()==',') tok.pop_back();
                unsigned int v = 0;
                std::stringstream hs;
                hs << std::hex << tok;
                hs >> v;
                bytes.push_back((uint8_t)(v & 0xff));
            }
        }

        // locate the object boundaries for this raw_frame so we can extract
        // optional metadata like "bad_type".
        size_t obj_start = s.rfind('{', pos);
        size_t obj_end = s.find('}', end);
        std::string bad;
        if (obj_start != std::string::npos && obj_end != std::string::npos && obj_end > obj_start) {
            std::string obj = s.substr(obj_start, obj_end - obj_start + 1);
            size_t bt = obj.find("\"bad_type\"");
            if (bt != std::string::npos) {
                size_t colon2 = obj.find(':', bt);
                if (colon2 != std::string::npos) {
                    size_t oq = obj.find('"', colon2);
                    if (oq != std::string::npos) {
                        size_t oq2 = obj.find('"', oq+1);
                        if (oq2 != std::string::npos) {
                            bad = obj.substr(oq+1, oq2 - (oq+1));
                        }
                    }
                }
            }
        }

        if (!bytes.empty()) out.push_back(FrameSpec{bytes, bad});
        pos = end + 1;
    }
    return out;
}

bool MctpFramer_decode_test() {
    auto frames = parse_raw_frames("./mctp_random_messages.json");
    if (frames.empty()) {
        std::cerr << "No frames parsed from JSON\n";
        return false;
    }

    // Limit the number of frames exercised to keep test-time reasonable
    size_t max_tests = std::min<size_t>(frames.size(), 50);

    // Create a single socketpair and framer and reuse them for all frames to
    // avoid per-frame thread startup/teardown costs (the framer starts a
    // background receive thread on openFd()).
    int pfd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pfd) < 0) {
        perror("socketpair");
        return false;
    }

    MctpFramer fr;
    int wake_fd = fr.openFd(pfd[0]);
    if (wake_fd < 0) {
        ::close(pfd[0]); ::close(pfd[1]);
        std::cerr << "openFd failed\n";
        return false;
    }
    // Enable diagnostics to log framer internals for debugging intermittent failures
    fr.show_diagnostics(false);

    bool ok = true;

    for (size_t i = 0; i < max_tests; ++i) {
        const FrameSpec &spec = frames[i];
        const auto &raw = spec.raw;
        const std::string &bad = spec.bad_type;
        if (util::isVerbose()) std::cerr << "MctpFramer_decode: processing frame " << i << " bad='" << bad << "' raw.size=" << raw.size() << "\n";
    // progress trace removed in final test
        if (raw.size() <= 6) {
            if (util::isVerbose()) std::cerr << "Skipping tiny frame\n";
            continue;
        }

        // expected decoded packet: strip first 3 bytes (FRAME, VER, LEN) and last 3 bytes (FCS hi, FCS lo, FRAME)
        std::vector<uint8_t> expected(raw.begin()+3, raw.end()-3);

        // reusing pfd/wake_fd and fr from above

    // encode the raw (unescaped) frame into the serial-encoded representation
        std::vector<uint8_t> enc;
        const uint8_t FRAME_CHAR = 0x7e;
        const uint8_t ESCAPE_CHAR = 0x7d;
        for (size_t bi = 0; bi < raw.size(); ++bi) {
            uint8_t data = raw[bi];
            // The framer transmitter does not escape the serial header (first
            // 3 bytes) or the serial FCS+trailer (last 3 bytes). Match that
            // behavior here so the test encodes frames exactly as an on-wire
            // transmitter would.
            if ((bi < 3) || (bi >= raw.size() - 3)) {
                enc.push_back(data);
            } else if ((data == FRAME_CHAR) || (data == ESCAPE_CHAR)) {
                enc.push_back(ESCAPE_CHAR);
                enc.push_back((uint8_t)(data - 0x20));
            } else {
                enc.push_back(data);
            }
        }

        // For corrupt_fcs test mutate the FCS bytes before encoding so the
        // transmitted frame has an invalid checksum.
        std::vector<uint8_t> raw_to_send = raw;
        if (bad == "corrupt_fcs") {
            if (raw_to_send.size() >= 4) raw_to_send[raw_to_send.size()-3] ^= 0xff;
        }

        // re-encode from raw_to_send
        enc.clear();
        for (size_t bi = 0; bi < raw_to_send.size(); ++bi) {
            uint8_t data = raw_to_send[bi];
            if ((bi < 3) || (bi >= raw_to_send.size() - 3)) {
                enc.push_back(data);
            } else if ((data == FRAME_CHAR) || (data == ESCAPE_CHAR)) {
                enc.push_back(ESCAPE_CHAR);
                enc.push_back((uint8_t)(data - 0x20));
            } else {
                enc.push_back(data);
            }
        }

        // write the encoded bytes to the write end
        // (no-op) previously had a temporary debug dump here
        if (bad == "partial") {
            // write only the first half to simulate a partial frame
            size_t half = enc.size() / 2;
            std::vector<uint8_t> first(enc.begin(), enc.begin() + half);
            if (!write_all_fd(pfd[1], first)) {
                std::cerr << "write failed\n";
                std::cerr << "MctpFramer_decode: write_all_fd(first) failed at frame " << i << "\n";
                ok = false; break;
            }
        } else {
            if (!write_all_fd(pfd[1], enc)) {
                    if (util::isVerbose()) std::cerr << "write failed\n";
                    std::cerr << "MctpFramer_decode: write_all_fd(enc) failed at frame " << i << "\n";
                    ok = false; break;
                }
        }

        if (bad == "partial") {
            // We already wrote the first half above; now do a short wait to
            // ensure no wake is signaled, then write the remainder and continue.
            fd_set shortfds;
            FD_ZERO(&shortfds);
            FD_SET(wake_fd, &shortfds);
            struct timeval shorttv{0, 200000}; // 200 ms
            int rsv = select(wake_fd + 1, &shortfds, nullptr, nullptr, &shorttv);
            if (rsv > 0) {
                std::cerr << "Unexpected wake on partial frame before completion\n";
                std::cerr << "MctpFramer_decode: unexpected wake on partial at frame " << i << "\n";
                ok = false; break;
            }
            // write remainder
            size_t half = enc.size() / 2;
            std::vector<uint8_t> rest(enc.begin() + half, enc.end());
            if (!write_all_fd(pfd[1], rest)) {
                std::cerr << "write failed (rest)\n";
                std::cerr << "MctpFramer_decode: write_all_fd(rest) failed at frame " << i << "\n";
                ok = false; break;
            }
        }

        // no debug peeks here; proceed to wait for framer wake

        // wait for the framer to write to the wake pipe
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(wake_fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sel = select(wake_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (bad == "corrupt_fcs") {
            // we expect no wake for corrupt frames
            if (sel > 0) {
                std::cerr << "Unexpected wake for corrupt frame\n";
                std::cerr << "MctpFramer_decode: unexpected wake for corrupt frame at " << i << "\n";
                ok = false; break;
            }
            // good — no packet should be produced
            continue;
        }

        if (sel <= 0) {
            // no wake; treat as failure
            std::cerr << "MctpFramer_decode: select timeout/wakeup failed at frame " << i << "\n";
            ok = false; break;
        }

        // read the packet directly from the framer's receive endpoint
        auto pkt = fr.receive();
        if (pkt != expected) {
            std::cerr << "Mismatch on frame " << i << ": expected size " << expected.size() << " got " << pkt.size() << "\n";
            std::cerr << "MctpFramer_decode: content mismatch at frame " << i << "\n";
            ok = false; break;
        }

        // continue to next frame
    }

    // Clean up shared resources
    fr.close();
    ::close(pfd[0]); ::close(pfd[1]);

    if (!ok) return false;
    return true;
}
