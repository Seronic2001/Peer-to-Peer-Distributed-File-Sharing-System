#pragma once

#include <string>
#include <vector>

// Sends a message over a TCP socket with a 4-byte length prefix.
// This ensures that the receiver can correctly frame the message.
bool sendMessage(int sockfd, const std::string& message);

// Receives a message from a TCP socket that has a 4-byte length prefix.
// This function will block until the entire message is received.
bool receiveMessage(int sockfd, std::string& message);
