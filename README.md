# MCTP-Over-Serial Library and Tools

![License](https://img.shields.io/github/license/PICMG/IoTorch)
![Coverage](https://img.shields.io/codecov/c/github/PICMG/IoTorch)
![Issues](https://img.shields.io/github/issues/PICMG/IoTorch)
![Forks](https://img.shields.io/github/forks/PICMG/IoTorch)
![Stars](https://img.shields.io/github/stars/PICMG/IoTorch)
![Last Commit](https://img.shields.io/github/last-commit/PICMG/IoTorch)


This repository contains tools, libraries, and example programs for running MCTP (Management Component Transport Protocol) over serial links on Linux. It bundles a small C++ framing library, a user-space bridge that connects AF_MCTP sockets to a serial transport, and kernel patching helpers for older kernels that require a routing fix.

This repository is part of the IoTFoundry family of open source projects.  For more information about IoTFoundry, please visit the main IoTFoundry site at: [https://picmg.github.io/iot-foundry/](https://picmg.github.io/iot-foundry/)

## Repository Resources
* Linux-mctp: Resources for linux userpsace development.  The README for this portion of the project.
    * [README](./README.md) - the detailed readme for linux installation for these tools.
    * *lib-mctp* - a lightweight c++ library for mctp-over serial support.  It includes ojbects for framing and bridging.
    * *mctp-bridge* - a userspace tool for instantiating a serial-backed mctp endpoint that supports broadcast messaging and remains persistent even if the underlying serial device is removed. More information on this tool can be found in its associated [README](./linux-mctp/mctp-bridge/README.md)
## System Requirements
The following are system requirements for MCTP communications over Linux supported by this project:

- Linux 6.17.11 or newer version.
- At least one serial port to serve as the MCTP bus.
  - USB with a Linux-compliant USB-serial adapter is sufficient.
- At least one MCTP hardware endpoint that can be connected to the serial (or serial over USB) port.
- Cabling to connect the endpoint to the serial (USB over serial) port.
- MCTP user tools and daemon from the [Code Construct Github respository](https://github.com/CodeConstruct/mctp) (optional)

Ensure the kernel has MCTP support enabled. This can be checked by examining /proc/net/protocols using the following linux shell command:
```
grep MCTP /proc/net/protocols
```
If MCTP is listed, the protocol is available and built into the kernel. 

Even if the operating system is built with MCTP enabled, the serial binding module may not be loaded by default.
This can be checked by typing the following shell command:
```
sudo lsmod | grep mctp
```
If you see no MCTP modules running, it will need to be loaded.  
```
sudo modprobe mctp_serial
```
to make loading of the module permanent, create an mctp_serial.conf file in the modules-load.d folder by executing the following:
```
sudo bash -c 'echo -e "mctp_serial\n" > /etc/modules-load.d/mctp_serial.conf'
```
The linux system is now ready to communicate over MCTP to serial endpoints.

## Build & Install

Follow these steps to build the library and (optionally) the application. The examples use an out-of-tree build directory.

- Create and configure the build directory (builds only the library by default):

```sh
mkdir -p build && cd build
cmake -S .. -B . -DBUILD_LIB=ON -DBUILD_APP=OFF -DBUILD_TESTS=OFF \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local
```

- Build and install (system-wide; requires `sudo`):

```sh
cmake --build . -j$(nproc)
sudo cmake --install .
```

- Install to a local prefix (no sudo):

```sh
cmake -S .. -B . -DBUILD_LIB=ON -DCMAKE_INSTALL_PREFIX=$HOME/.local
cmake --build . -j$(nproc)
cmake --install .
```

- Build and include tests (optional):

```sh
cmake -S .. -B . -DBUILD_LIB=ON -DBUILD_TESTS=ON
cmake --build . -j$(nproc)
sudo ctest --output-on-failure    # some tests require root
```

Files installed by the project (under `<prefix>`):

- `lib/libsermctp.so*` (shared library)
- `lib/libsermctp_static.a` (if `-DBUILD_STATIC_LIBS=ON`)
- `include/sermctp/*` (public headers)
- `lib/pkgconfig/sermctp.pc`
- `lib/cmake/sermctp/` (CMake export files)

Verification examples

- With pkg-config:

```sh
pkg-config --cflags --libs sermctp
pkg-config --modversion sermctp
```

- From another CMake project:

```cmake
find_package(sermctp CONFIG REQUIRED)
target_link_libraries(myexe PRIVATE sermctp::sermctp)
```

Staged/package install

To stage files for packaging without writing to system locations use `DESTDIR` or a staging prefix:

```sh
# example: create a staged tree under /tmp/stage
cmake -S .. -B build -DCMAKE_INSTALL_PREFIX=/usr
sudo cmake --build build --target install -- -j$(nproc) DESTDIR=/tmp/stage
```

Uninstall

The build does not provide an automatic uninstall target. Use staged installs for packaging, or remove installed files manually (check `install_manifest.txt` if generated).

## Additional References
The following may be useful resources for this project.

- [Linux MCTP Documentation](https://docs.kernel.org/networking/mctp.html): The documentation for the Linux MCTP sockets API.
- [MCTP Specification](https://www.dmtf.org/dsp/DSP0236) Defines the Management Component Transport Protocol.
- [MCTP Serial Transport Binding Specification](https://www.dmtf.org/documents/pmci/mctp-serial-transport-binding-specification-100) Defines the Management Component Transport Binding Protocol.




