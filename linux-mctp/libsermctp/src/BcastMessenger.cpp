/**
 * @file BcastMessenger.cpp
 * @brief Class for managing Unix domain socket-based broadcast messaging.
 *
 * This class provides a lightweight interface for setting up a Unix domain socket
 * server using sequenced packet communication. It supports binding to a socket path,
 * accepting a single client connection, and sending/receiving framed messages.
 *
 * The socket operates in non-blocking mode and is designed for simple broadcast-style
 * messaging between local processes. It automatically cleans up stale socket files
 * and handles client disconnects gracefully.
 *
 * Typical usage includes:
 * - Setting up a socket listener with `setup()`
 * - Accepting a client with `acceptConnection()`
 * - Exchanging messages using `recvMessage()` and `sendMessage()`
 * - Cleaning up resources with `cleanup()`
 *
 * @author Doug Sandy (doug@picmg.org)
 * @license MIT No Attribution (MIT-0)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
 */
#include "BcastMessenger.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>  // for fcntl
#include <cstring>
#include <iostream>
#include <stdexcept>

/**
 * @brief Constructor for BcastMessenger.
 */
BcastMessenger::BcastMessenger()
    : listenFd(-1), connFd(-1), state(State::Idle) {}

/**
 * @brief Destructor for BcastMessenger. Cleans up resources.
 */
BcastMessenger::~BcastMessenger() {
    close();
}

/**
 * @brief Sets up the Unix domain socket, binds it, and starts listening.
 * @param path The Unix domain socket path to bind to.
 * @return false if socket creation, binding, or listening fails. Otherwise, true
 */
bool BcastMessenger::open(const std::string& path) {
    // Create a sequenced packet socket
    listenFd = socket(AF_UNIX, SOCK_SEQPACKET, 0); 
    if (listenFd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    // Remove any stale socket file
    unlink(path.c_str()); 

    // attempt to bind to the socket file
    if (bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd);
        listenFd = -1;
        return false;
    }
 
    state = State::Bound;

    if (listen(listenFd, 5) < 0) {
        // timeout when transitioning to listen mode
        ::close(listenFd);
        listenFd = -1;
        return false;
    }

    state = State::Listening;

    // create a fncntl to avoid blocking on accept connection.
    int flags = fcntl(listenFd, F_GETFL, 0);
    fcntl(listenFd, F_SETFL, flags | O_NONBLOCK);
    socketPath = path;
    return true;
}

/**
 * @brief Returns the socket path the messenger is bound to.
 * @return The Unix domain socket path as a string.
 */
std::string BcastMessenger::getSocketPath() const {
    return socketPath;
}

/**
 * @brief Accepts a single incoming client connection.
 * @throws std::runtime_error if accept fails.
 */
void BcastMessenger::acceptConnection() {
    connFd = accept(listenFd, nullptr, nullptr);
    if (connFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;  // No pending connection — non-blocking behavior
        }
        throw std::runtime_error("accept() failed");
    }
    state = State::Connected;
}

/**
 * @brief Returns the file descriptor of the connected client socket.
 * @return File descriptor of the connected socket, or -1 if not connected.
 */
int BcastMessenger::getFd() const {
    return connFd;
}

/**
 * @brief Checks whether a client is currently connected.
 * @return True if connected, false otherwise.
 */
bool BcastMessenger::isConnected() const {
    return state == State::Connected && connFd >= 0;
}

/**
 * @brief Receives a message from the connected client.
 * @return A vector of bytes containing the message, or empty if no message.
 */
std::vector<uint8_t> BcastMessenger::recvMessage() {
    std::vector<uint8_t> result;

    if (!isConnected()) return result;

    uint8_t buf[256];
    ssize_t len = recv(connFd, buf, sizeof(buf), MSG_DONTWAIT); // Non-blocking receive
    if (len <= 0) {
        if (len == 0 || (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            // Client closed connection or a non-transient error occurred.
            // Handle silently: drop connection and return empty result so
            // callers treat it as no message available. This avoids noisy
            // diagnostic output for normal client shutdowns.
            ::close(connFd);
            connFd = -1;
            state = State::Listening;
        }
        return result; // Return empty vector
    }

    result.insert(result.end(), buf, buf + len); // Copy received bytes into vector
    return result;
}

/**
 * @brief Sends a message to the connected client.
 * @param data A vector of bytes to send.
 */
void BcastMessenger::sendMessage(const std::vector<uint8_t>& data) {
    if (!isConnected()) return;

    ssize_t sent = send(connFd, data.data(), data.size(), 0); // Send data
    if (sent < 0) {
        std::cerr << "send() failed\n";
    }
}

/**
 * @brief Cleans up socket resources and removes the socket file.
 */
void BcastMessenger::close() {
    if (connFd >= 0) ::close(connFd);     // Close client connection
    if (listenFd >= 0) ::close(listenFd); // Close listening socket
    unlink(socketPath.c_str());         // Remove socket file from filesystem
}