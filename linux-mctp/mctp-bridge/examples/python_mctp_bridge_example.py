#!/usr/bin/env python3
"""
Interactive mctp-bridge example (Python)
---------------------------------------
This script spawns `mctp-bridge` with a specified local_eid and remote_eid.
Characters typed at the keyboard are sent as AF_MCTP datagrams to the bridge's 
remote EID (9) and incoming MCTP datagrams are printed to the standard output
device.

NOTES:
'mctp-bridge' must be in your PATH for this script to work.
The script requires root privileges to create AF_MCTP sockets.

Usage: run from repository root:
    python3 mctp-bridge/examples/python_mctp_bridge_example.py --tty /dev/pts/X --local-eid 8 --remote-eid 9
    python3 mctp-bridge/examples/python_mctp_bridge_example.py --id-path-tag "pci-0000:00:14.0-usb-0:3:1.0" --local-eid 8 --remote-eid 9

Author: Doug Sandy <doug@picmg.org>
License: MIT No Attribution (MIT-0)
"""

import os
import subprocess
import time
import argparse
import struct
import socket
import select
import sys
import tty
import termios
import ctypes
import ctypes.util
import errno

# AF_MCTP example sockets: send datagrams to EID 9, listen on a local EID
AF_MCTP = 45

# load libc and define helpers - ctypes are used in case python not built with AF_MCTP support.
libc_name = ctypes.util.find_library("c") or "libc.so.6"
libc = ctypes.CDLL(libc_name, use_errno=True)
libc.socket.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
libc.socket.restype = ctypes.c_int
libc.bind.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
libc.bind.restype = ctypes.c_int
libc.sendto.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint]
libc.sendto.restype = ctypes.c_ssize_t
libc.recvfrom.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
libc.recvfrom.restype = ctypes.c_ssize_t
libc.close.argtypes = [ctypes.c_int]
libc.close.restype = ctypes.c_int

# helper function for C socket bind - not required if using standard python sockets.
def c_bind_fd(fd, packed):
    buf = ctypes.create_string_buffer(packed)
    res = libc.bind(fd, ctypes.byref(buf), ctypes.c_uint(len(packed)))
    if res != 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))

# helper function for C socket sendto - not required if using standard python sockets.
def c_sendto(fd, data, addr_packed):
    buf = ctypes.create_string_buffer(data)
    addrbuf = ctypes.create_string_buffer(addr_packed)
    res = libc.sendto(fd, ctypes.byref(buf), ctypes.c_size_t(len(data)), 0, ctypes.byref(addrbuf), ctypes.c_uint(len(addr_packed)))
    if res < 0:
        err = ctypes.get_errno()
        raise OSError(err, os.strerror(err))
    return res

# helper function for C socket recvfrom - not required if using standard python sockets.
def c_recvfrom(fd, maxlen):
    buf = ctypes.create_string_buffer(maxlen)
    addrbuf = ctypes.create_string_buffer(64)
    addrlen = ctypes.c_uint(ctypes.sizeof(addrbuf))
    res = libc.recvfrom(fd, ctypes.byref(buf), ctypes.c_size_t(maxlen), 0, ctypes.byref(addrbuf), ctypes.byref(addrlen))
    if res < 0:
        err = ctypes.get_errno()
        raise OSError(err, errno.errorcode.get(err, str(err)))
    return buf.raw[:res], addrbuf.raw[:addrlen.value]

# Define a ctypes.Structure that matches the C `struct sockaddr_mctp` layout
class SockAddrMCTP(ctypes.Structure):
    class InAddr(ctypes.Structure):
        _fields_ = [
            ("s_addr", ctypes.c_ubyte),
        ]
    _fields_ = [
        ("smctp_family", ctypes.c_ushort),
        ("__smctp_pad0", ctypes.c_ushort),
        ("smctp_network", ctypes.c_uint),
        ("smctp_addr", InAddr),
        ("smctp_type", ctypes.c_ubyte),
        ("smctp_tag", ctypes.c_ubyte),
        ("__pad1", ctypes.c_ubyte)
    ]

# Function to pack sockaddr_mctp structure
def pack_sockaddr_mctp(eid, typ=1, tag=0, network=0, family=AF_MCTP):
    s = SockAddrMCTP()
    s.smctp_family = family
    s.__smctp_pad0 = 0
    s.smctp_network = network
    s.smctp_addr.s_addr = eid & 0xff
    s.smctp_type = typ & 0xff
    s.smctp_tag = tag & 0xff
    s.__pad1 = 0
    raw = ctypes.string_at(ctypes.addressof(s), ctypes.sizeof(s))
    # ensure the packed length is at least 12 bytes (kernel expects sockaddr-sized structs)
    if len(raw) < 12:
        raw = raw + (b"\x00" * (12 - len(raw)))
    return raw

# set up the main interactive loop and loop until ctrl-c typed
def run():
    parser = argparse.ArgumentParser(description="AF_MCTP interactive example")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--tty", help="TTY path to pass to mctp-bridge (e.g. /dev/ttyUSB0)")
    group.add_argument("--id-path-tag", help="udev ID_PATH_TAG to monitor instead of a tty path")
    parser.add_argument("--local-eid", type=int, default=8, help="Local EID to bind/send from (default: 8)")
    parser.add_argument("--remote-eid", type=int, default=9, help="Remote EID to send datagrams to (default: 9)")
    args = parser.parse_args()

    # check for sudo.
    if os.geteuid() != 0:
        print("Re-executing under sudo for privileged operations...")
        os.execvp("sudo", ["sudo", sys.executable] + sys.argv)

    # launch the bridge.
    bridge_exe = "mctp-bridge"
    proc = None
    # Determine whether to launch bridge with a tty path or an ID_PATH_TAG
    if args.id_path_tag:
        chosen_device = args.id_path_tag
        cmd = [bridge_exe, "--id-path-tag", chosen_device, "--local-eid", str(args.local_eid), "--remote-eid", str(args.remote_eid)]
    else:
        chosen_device = args.tty
        cmd = [bridge_exe, "--tty", chosen_device, "--local-eid", str(args.local_eid), "--remote-eid", str(args.remote_eid)]
    print("Launching mctp-bridge...")
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)    
    except Exception as e:
        print(f"Failed to launch mctp-bridge: {e}", file=sys.stderr)
    time.sleep(5)  # brief pause to allow bridge to initialize

    # create AF_MCTP sockets using libc
    bridge_fd = libc.socket(AF_MCTP, socket.SOCK_DGRAM, 0)
    if bridge_fd < 0:
        print("Failed to create AF_MCTP sockets via libc", file=sys.stderr)
        if bridge_fd >= 0: libc.close(bridge_fd)
        if proc:
            proc.terminate()
        sys.exit(1)

    # set the sockaddr_mctp for the socket - bindding it to local EID
    rx_sockaddr = pack_sockaddr_mctp(args.local_eid, typ=1, tag=0)
    try:
        print("Attempting to bind socket...")
        c_bind_fd(bridge_fd, rx_sockaddr)
        if (bridge_fd>= 0):
            print("Socket bound successfully.")
    except Exception as e:
        print("Failed to bind socket:", e, file=sys.stderr)
        if bridge_fd >= 0: libc.close(bridge_fd)
        if proc:
            proc.terminate()
        sys.exit(1)

    # create an address for our destination message sends
    dest_eid = args.remote_eid
    dest_sockaddr = pack_sockaddr_mctp(dest_eid, typ=1, tag=0x8)

    print(f"Interactive mode: type characters to send as AF_MCTP datagrams to EID {dest_eid}. Ctrl-C to exit.")

    # Put stdin into raw mode so we get single characters
    old_settings = termios.tcgetattr(sys.stdin.fileno())
    try:
        tty.setraw(sys.stdin.fileno())
        while True:
            rlist, _, _ = select.select([sys.stdin.fileno(), bridge_fd], [], [])
            if sys.stdin.fileno() in rlist:
                ch = os.read(sys.stdin.fileno(), 1)
                if not ch:
                    break
                # If user typed Ctrl-C (ETX, 0x03) treat as KeyboardInterrupt
                if ch == b'\x03':
                    raise KeyboardInterrupt()
                # Echo the typed character so user sees it in raw mode
                try:
                    os.write(sys.stdout.fileno(), ch)
                    sys.stdout.flush()
                except Exception:
                    pass
                # send the character as an MCTP datagram to the destination EID
                try:
                    payload = ch
                    sent = c_sendto(bridge_fd, payload, dest_sockaddr)
                except Exception as e:
                    try:
                        print(f"\nSEND ERROR -> {e}", file=sys.stderr)
                    except Exception:
                        pass
            if bridge_fd in rlist:
                data, addr = c_recvfrom(bridge_fd, 4096)
                # Write the decoded text payload to stdout so it appears inline
                try:
                    text = data.decode('utf-8', errors='replace')
                    sys.stdout.write(text)
                    sys.stdout.flush()
                except Exception:
                    pass
    except KeyboardInterrupt:
        print("\nInterrupted by user")
        # ask bridge to terminate cleanly
        try:
            if proc:
                proc.send_signal(2)  # SIGINT
        except Exception:
            pass
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_settings)
        try:
            if bridge_fd >= 0:
                libc.close(bridge_fd)
        except Exception:
            pass
        if proc:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == '__main__':
    run()