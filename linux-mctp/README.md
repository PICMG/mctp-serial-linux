# IoTorch MCTP Over Linux Installation and Setup

This repository contains tools, libraries, and example programs for running MCTP (Management Component Transport Protocol) over serial links on Linux. It bundles a small C++ framing library, a user-space bridge that connects AF_MCTP sockets to a serial transport, and kernel patching helpers for older kernels that require a routing fix.

**Subprojects**

- `libsermctp/` — C++ library implementing MCTP serial framing, reassembly, and helpers. See `libsermctp/README.md` for build instructions and examples.
- `mctp-bridge/` — user-space bridge that exposes a kernel-facing AF_MCTP interface and a broadcast socket while forwarding frames to/from a serial device. See `mctp-bridge/README.md` for build and runtime instructions.
- `patches/` — kernel patching helpers, scripts, and the patch guide. See `patches/PATCHING_THE_KERNEL.md` for details on when and how to apply kernel fixes.

## System Requirements

The following are system requirements for the mctp communications over linux supported by IoTorch:
- Linux 6.17 with patch applied.  Although Linux 5.17 and beyond includes MCTP support for MCTP_SERIAL and MCTP core functions, however multiple improvements were made since that time. Currently a patch is still required to support proper routing of MCTP serial messages between TTY endpoints.Future kernel updates are expected to fix this issue.
- At least one serial port to serve as the MCTP bus.
  - USB with a Linux-compliant USB-serial adapter is sufficient.
- At least one MCTP hardware endpoint that can be connected to the serial (or serial over USB) port.
- Cabling to connect the endpoint to the serial (USB over serial) port.
- MCTP user tools and daemon from the [Code Construct Github respository](https://github.com/CodeConstruct/mctp) (optional)

## Software Setup
This section details setup of software for IoTorch Linux tools.
### Linux installation
Follow the instructions in the [Ubuntu Documentation](https://canonical-ubuntu-desktop-documentation.readthedocs-hosted.com/en/latest/tutorial/install-ubuntu-desktop/) 
to create a new bootable Linux image using Ubuntu 25.10, or the most recent LTS version if newer versions are available.

### Linux Setup
Ensure your kernel has MCTP support enabled. You can check this by examining /proc/net/protocols using the following 
linux shell command:
```
grep MCTP /proc/net/protocols
```
If MCTP is listed, the protocol is available and built into the kernel. 

Even if your operating system is built with MCTP enabled, the serial binding module may not be loaded by default.
You can check to see if the module is loaded by typing the following shell command:
```
sudo lsmod : grep mctp
```
If you see no MCTP modules running, you will need to load the serial module.  Fortunately, this is easy to do with the following command:
```
sudo modprobe mctp_serial
```
to make loading of the module permanent, create an mctp_serial.conf file in the modules-load.d folder by executing the following:
```
sudo bash -c 'echo -e "mctp_serial\n" > /etc/modules-load.d/mctp_serial.conf'
```
The linux system is now ready to communicate over MCTP to serial endpoints.

### Patching the kernel
For kernels that exhibit the routing/header mismatch described above you may
need to apply a small, focused source patch. Detailed, copy-pasteable
instructions, the functional test used to detect the problem and helper scripts
are included in the `patches/` directory. See the patching guide here:

[Patching the Kernel](./patches/PATCHING_THE_KERNEL.md)

### Building the Command Line Tools (Optional)
Once linux is configured, you will need to build and install the userspace tools for MCTP support. These can be found on
build the mctp tools from the [Code Construct GitHub respository](https://github.com/CodeConstruct/mctp).
Brief documentation for these tools can be found in the README.md file on the repository.  IoTorch will use these tools for the following purposes:
- **mctp** - establish socket-capable links to specific serial port and expose them to the mctpd tool.
- **mctpd** - manage endpoint discovery and routing table management.  This tool is normally used as a daemon, but can also be invoked at the Linux command line.

To begin the installation process, first install any missing dependencies with the following:
```
sudo apt update
sudo apt install -y meson ninja-build python3 python3-pytest dbus-x11
```
Next download the source image using:
```
wget https://github.com/CodeConstruct/mctp/archive/refs/heads/main.tar.gz -O mctp-main.tar.gz
tar -xvzf mctp-main.tar.gz
cd mctp-main
```
Once the tools source is downloaded, build and install the tools as follows:
```
sudo meson setup obj -Dtests=false
sudo ninja -C obj
sudo meson install -C obj
```
### Installing the Daemon
This guide walks you through configuring mctpd as a systemd-managed service without enabling it to start automatically on boot.
#### Create a Configuration File
Create or edit your configuration file:
```
sudo nano /etc/mctpd.conf
```
You can base it on the sample from the CodeConstruct GitHub repository. Make sure it reflects your hardware setup and serial devices.  
The following minimal configuration can be used if all serial interfaces will be added later:
```
mode = "bus-owner"

[mctp]
message_timeout_ms = 30

[bus-owner]
dynamic_eid_range = [8, 254]
max_pool_size = 15
```
#### Create the systemd Service File
Create a new systemd unit file:
```
sudo nano /etc/systemd/system/mctpd.service
```

Paste the following content:
```
[Unit]
Description=MCTP Daemon
After=network.target

[Service]
ExecStart=/usr/local/sbin/mctpd -c /etc/mctpd.conf
Restart=always
User=root
Group=root

[Install]
WantedBy=multi-user.target
```
Note: Adjust the ExecStart path if your mctpd binary is located elsewhere (e.g., /usr/bin/mctpd).

### Reload systemd
Reload systemd to recognize the new service:
```
sudo systemctl daemon-reexec
sudo systemctl daemon-reload
```

### Testing the Installation
Testing of the installation can be done in two steps:
1. Verify that a serial connection can be established as a MCTP bus.
2. Verify that the MCTP daemon can identify connect to the bus and enumerate any connections.
#### Verify Serial Connection
Open a command window and establish a link to a serial port (if using serial-over-USB, the USB-to-serial device must be plugged in).  Use the following command:
```
sudo mctp link serial <device-path>
```
<device-path> should be replaced by the full path to your device (e.g. /dev/ttyUSB0). 
**If everything works properly, this process will block. Do not interrupt the process, the serial port will only be available to MCTP as long as the process is running.**

To verify that a new mctp-compatible serial link has been created, issue the following command in a **new terminal window**:
```
ip link
```
If you see a link with a name like 'mctpserial0' listed as a link, the link has been successfully created.
#### Verify MCTP Daemon Function
While the mctpd can be installed as a daemon, IoTorch currently launches it from the command line.  This simplifies configuration but does mean that it must be launched manually every time it is used.

Before running mctpd, the previously established mctp serial link must be given eid (mctp address) and powered-up.  
Use the following commands, replacing <mctpserial-name> with the name of the serial link that was created by the mctp tool.
```
sudo mctp link set <mctpserial-name> up
sudo mctp address add 8 dev <mctpserial-name>
```
Note, if more than one serial port link is added, each must be given a unique eid.

### Start the Service Manually
Start the service without enabling it at boot:
```
sudo systemctl start mctpd.service
sudo systemctl status mctpd.service
```
You should see Active: active (running) if everything is working correctly.

At this point, you may stop the service and stop the process that created the serial port link.
