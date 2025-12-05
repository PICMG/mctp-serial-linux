/**
 * @file BcastMessenger.hpp
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
#pragma once
#include <string>
#include <vector>
#include <cstdint>

class BcastMessenger {
public:
    enum class State {
        Idle,
        Bound,
        Listening,
        Connected
    };

    // Constructor: takes the socket path to bind to
    BcastMessenger();

    // Destructor: cleans up file descriptors and socket file
    ~BcastMessenger();

    // Sets up the listening socket and binds it to the path
    bool open(const std::string& path);

    // Internal cleanup logic for sockets and file
    void close();

    // Accepts a single incoming connection
    void acceptConnection();

    // Returns the connected socket file descriptor
    int getFd() const;

    // Returns true if a client is connected
    bool isConnected() const;

    // Receives a message from the connected client.
    std::vector<uint8_t> recvMessage();

    // Sends a message to the connected client.
    void sendMessage(const std::vector<uint8_t>& data);

    std::string getSocketPath() const;
private:
    std::string socketPath;
    int listenFd;
    int connFd;
    State state;

};

