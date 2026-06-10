#pragma once

#include <winsock2.h>
#include <thread>
#include <string>
#include <stdexcept>
#include "sharedData/sharedDataClient.h"

class NetworkManagerClient
{
public:
    NetworkManagerClient(SharedDataClient& shared)
        : shared_(shared) {}

    ~NetworkManagerClient() { disconnect(); }

void connect(const std::string& ip, int port);
    void disconnect();
    void sendMessage(const std::string& message);
    void sendImage(const std::string& filepath);
    void setUsername(const std::string& name); 
    void sendWho    ();

    bool isConnected() const { return shared_.connected.load(); }
    std::string getUsername() const { return shared_.username; }

private:
    SharedDataClient& shared_;
    std::thread       recv_thread_;
    std::thread       send_image_thread_;

    void recvThread();   // runs in background, feeds shared data
    void sendImageThread(const std::string& filepath); // runs in background, sends image data
};