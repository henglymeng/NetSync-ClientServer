#pragma once

#include <winsock2.h>
#include <thread>
#include <string>
#include <stdexcept>
#include "sharedData/sharedDataServer.h"

class NetworkManager
{
public:
    NetworkManager(SharedData& shared, int port)
        : shared_(shared), port_(port), ip_("0.0.0.0") {}

    ~NetworkManager() { stop(); }

    void start();
    void start(const std::string& ip, int port);  // ← overload with IP+port
    void stop();
    void broadCasting(const std::string& message);
    void syncOnlineUsersToAll(NetworkManager& netManager, SharedData& shared);

    bool        isRunning()  const { return shared_.running.load(); }
    std::string getIP()      const { return ip_;   }
    int         getPort()    const { return port_; }

private:
    SharedData& shared_;
    int         port_;
    std::string ip_;
    SOCKET      listen_sock_           = INVALID_SOCKET;
    std::thread accept_thread_;
    std::thread packet_counter_thread_;
};