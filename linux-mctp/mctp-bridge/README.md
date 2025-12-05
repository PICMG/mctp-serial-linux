# MCTP Bridge

This repository contains a user-space MCTP bridge that connects the Linux kernel MCTP subsystem, user-space applications, and a physical serial transport. The bridge translates between three logical ports: a kernel-facing unicast interface (used by AF_MCTP sockets), a user-space broadcast socket (AF_UNIX datagram), and a serial device that carries MCTP frames on the wire. The bridge is intended to let applications participate in MCTP unicast and broadcast exchanges without handling framing, checksums, or low-level link details.

## Architecture (high level)

The bridge sits in user space with three logical links:

```
+--------------------------------+        +----------------------+        +----------------------+
|  Port 1: Kernel MCTP           |◄──A───►|   Bridge (User Space)|◄──────►| Port 2: Serial Device|
|  Interface: mctpserial<x>      |        |                      |        | (e.g. /dev/ttyUSB0)  |
+--------------------------------+        +----------------------+        +----------------------+
                                                  ▲
                                                B │
                                                  ▼
                                 +--------------------------------+
                                 | Port 3: Broadcast Interface    |
                                 | Interface: bcast_mctpserial<x> |
                                 +--------------------------------+
```

- Port 1 — Kernel MCTP: a kernel-visible `mctp-serial<N>` interface. Kernel-land AF_MCTP sockets send unicast packets here and the kernel routes packets to the correct interface based on destination EID.  Optionally this interface may be renamed by command line arguments when the mctp-bridge is instantialed.
- Port 2 — Serial Transport: the physical serial device (e.g. `/dev/ttyUSB0`). The bridge encodes and decodes MCTP frames to/from this link.
- Port 3 — Broadcast interface that user-space processes use to send and receive broadcast MCTP messages via the bridge.  The name of this interface always matches the name of the kernel-facing interface, but prepended by `bcast`

These three pieces let user processes talk to remote MCTP endpoints (unicast) via the kernel stack and participate in broadcast exchanges via the bridge's UNIX datagram socket.

## User view

For most users the bridge is an application you run against a serial device. When started, the bridge:

- creates a kernel-visible `mctp-serial<N>` (or user-named) interface for unicast MCTP traffic (this is used by AF_MCTP sockets), and
- creates a UNIX datagram interface `bcast_mctp-serial<N>` (or `bcast_<kernel interface name>` for user-space broadcast messages.

### Unicast flow (what an application sees):

Applications create AF_MCTP sockets and bind to a local EID. When they send a unicast packet to a remote EID that is reachable via the bridge's kernel-facing interface, the kernel forwards the packet to the bridge, which transmits it over the serial link. Replies arrive back through the serial transport and are forwarded into the kernel/AF_MCTP path.

### Broadcast flow (user-space broadcast):
Applications that want to send broadcast MCTP messages use a `AF_UNIX`/`SOCK_DGRAM` socket and `sendto()` the bridge's broadcast interface. The bridge transmits the datagram as an MCTP broadcast on the serial bus. Any device that responds will have the response relayed back to the sender (if the sender had a bound socket address).

## Python AF_MCTP example

There is an interactive Python example that demonstrates direct use of AF_MCTP datagram sockets from user space and also launches the `mctp-bridge` binary for convenience:

- `mctp-bridge/examples/python_mctp_bridge_example.py`

Notes:

- The example uses libc via `ctypes` to create and bind AF_MCTP sockets and therefore requires root privileges for some operations (the script will re-exec itself under `sudo` if not run as root).
- Typical usage:

```bash
python3 mctp-bridge/examples/python_mctp_bridge_example.py --tty /dev/ttyUSB0 --local_eid 8 ==remote_eid 9
```
See the top of the example script for more detailed usage notes and command-line flags.

### Usage — quick example (run as root or via sudo):

```bash
sudo ./mctp-bridge --tty /dev/ttyUSB0 --ifname testbridge --baud B115200 --local-eid 8 --remote_eid 9 
```

## Developer view

The implementation handles framing, reassembly, and routing between three transports. Key responsibilities include:

- decoding and encoding MCTP frames to/from the serial line,
- forwarding unicast messages between the kernel and the serial transport,
- handling user-space broadcast datagrams on the AF_UNIX socket and mapping sender addresses for replies,
- ensuring correct behavior under blocking I/O and clean shutdown.

### Prerequisites

- Linux (Debian/Ubuntu, Fedora, etc.)
- C++17-capable compiler (g++ or clang)
- CMake >= 3.16
- pkg-config
- libnl development packages (names vary by distro): on Debian/Ubuntu install `libnl-3-dev libnl-genl-3-dev libnl-route-3-dev`; on Fedora `libnl3-devel`.

(Note: `pkg-config` must be able to locate the `libnl` `.pc` files; see Troubleshooting below if CMake cannot find `libnl-3`, `libnl-genl-3` or `libnl-route-3`.)

### Quick build (out-of-tree)

The CMakeLists for the bridge lives under `src/`. From the `mctp-bridge/` directory use an out-of-tree build that points at `src/` so the build files are created in `build/` and the source tree is left untouched.

```bash
# install deps (example for Debian/Ubuntu)
sudo apt update
# ensure pkg-config and apt-file are present; apt-file helps locate which package provides a given file
sudo apt install build-essential cmake pkg-config apt-file libnl-3-dev libnl-genl-3-dev libnl-route-3-dev

# (optional) initialize apt-file database so you can locate files provided by packages
sudo apt-file update

# create an out-of-tree build and configure (from mctp-bridge/)
cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# build the project
cmake --build build -- -j$(nproc)

# run the built binary (example)
sudo build/mctp-bridge --mode BRIDGE --tty /dev/ttyUSB0 --baud B115200
```

### Build with static `libsermctp` (recommended to avoid runtime shared-lib errors)

The `mctp-bridge` CMake lists prefer linking the static `libsermctp_static.a` if it is present under the `libsermctp/build/` directory. If you see an error like:

```
mctp-bridge: error while loading shared libraries: libsermctp.so.1: cannot open shared object file: No such file or directory
```

it means the bridge was linked against the shared library (or the shared library is not installed into a system library path). To build the bridge so it links the static archive instead, build `libsermctp` with the `BUILD_STATIC_LIBS` option enabled and then rebuild the bridge:

From linux-mctp/ run the following:

```bash
# build libsermctp (enable static archive)
cd ./libsermctp
mkdir -p build && cd build
cmake -S ../src -B . -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_STATIC_LIBS=ON
cmake --build . -- -j$(nproc)

# now build the bridge (it will detect and link the static archive)
cd ../../mctp-bridge
cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- -j$(nproc)

# run the in-tree bridge binary
sudo build/mctp-bridge --tty /dev/ttyUSB0 --baud B115200
```

Notes:

- The bridge's CMake logic prefers the static archive when it exists at `libsermctp/build/libsermctp_static.a`. Building `libsermctp` with `-DBUILD_STATIC_LIBS=ON` creates that file.
- Alternatively, install the shared `libsermctp` into a system library path (e.g. `/usr/local/lib`) and run `sudo ldconfig` so the runtime loader can find `libsermctp.so.1`.
- If you prefer the bridge to use the shared library from the build tree without installing, you can set `LD_LIBRARY_PATH` when running, for example:

```bash
LD_LIBRARY_PATH=../libsermctp/build sudo build/mctp-bridge --mode BRIDGE --tty /dev/ttyUSB0
```

but the static-link route avoids needing runtime library path tweaks and is recommended for simple developer workflows.

### Install (optional)

To make the `mctp-bridge` binary available system-wide (so scripts like the Python example can find it on `PATH`), install the built artifacts into the system prefix. From the `mctp-bridge/` directory:

```bash
# build as above (if not already built)
cmake -S src -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -- -j$(nproc)

# install into the system prefix (requires sudo)
sudo cmake --install build

# verify the binary is on PATH
which mctp-bridge || ls -l /usr/local/bin/mctp-bridge
```

The default install prefix is `/usr/local`. If you wish to install elsewhere, pass `-DCMAKE_INSTALL_PREFIX=/your/prefix` to the initial `cmake` configure command.

If you prefer not to install globally, run examples by pointing them at the in-tree built binary (e.g. `mctp-bridge/build/mctp-bridge`) or create a local symlink into `/usr/local/bin` as needed.

