#include "Thread/networkManagerServer.h"
#include "Thread/acceptThread.h"

#include <ws2tcpip.h>
#include <stdexcept>
#include <cstdio>

// ── Original start (uses stored ip_ and port_) ────────────────────
void NetworkManager::start()
{
    start(ip_, port_);
}

// ── Start with specific IP + port ─────────────────────────────────
void NetworkManager::start(const std::string& ip, int port)
{
    if (shared_.running)
    {
        printf("[NetworkManager] Already running.\n");
        return;
    }

    ip_   = ip;
    port_ = port;

    listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock_ == INVALID_SOCKET)
        throw std::runtime_error("socket() failed: "
            + std::to_string(WSAGetLastError()));

    // Allow socket reuse — prevents "address already in use" on restart
    int opt = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port_);

    // "0.0.0.0" = listen on all interfaces
    if (ip_ == "0.0.0.0" || ip_.empty())
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("Invalid IP: " + ip_);

    if (bind(listen_sock_,
             reinterpret_cast<sockaddr*>(&addr),
             sizeof(addr)) == SOCKET_ERROR)
        throw std::runtime_error("bind() failed: "
            + std::to_string(WSAGetLastError()));

    if (listen(listen_sock_, SOMAXCONN) == SOCKET_ERROR)
        throw std::runtime_error("listen() failed: "
            + std::to_string(WSAGetLastError()));

    shared_.running = true;

    printf("[NetworkManager] Listening on %s:%d\n",
           ip_.c_str(), port_);

    // ── Accept thread ─────────────────────────────────────────────
    accept_thread_ = std::thread(acceptThread,
                                 listen_sock_,
                                 std::ref(shared_));

    // ── Packet counter thread ─────────────────────────────────────
    packet_counter_thread_ = std::thread([this]()
    {
        while (shared_.running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int count = shared_.packetAccum.exchange(0);
            shared_.packetPerSecond.store(count);
        }
    });
}

// ── Stop ──────────────────────────────────────────────────────────
void NetworkManager::stop()
{
    if (!shared_.running) return;

    shared_.running = false;

    if (listen_sock_ != INVALID_SOCKET)
    {
        closesocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    if (packet_counter_thread_.joinable())
        packet_counter_thread_.join();

    printf("[NetworkManager] Stopped.\n");
}

// ── Broadcast ─────────────────────────────────────────────────────
void NetworkManager::broadCasting(const std::string& message)
{
    std::lock_guard<std::mutex> lock(shared_.mtx);

    for (const auto& c : shared_.clientList)
    {
        ::send(c.sock, message.c_str(),
               static_cast<int>(message.size()), 0);
    }

    shared_.messageLog.push_back("[BROADCAST] " + message);
    shared_.newDataReady = true;
}

void syncOnlineUsersToAll(NetworkManager& netManager, SharedData& shared)
{
    std::string listMsg = "ONLINE_LIST:";
    
    {
        std::lock_guard<std::mutex> lock(shared.mtx);
        for (size_t i = 0; i < shared.clientList.size(); ++i) {
            std::string name = shared.clientList[i].username.empty() 
                               ? "Client" + std::to_string(shared.clientList[i].id) 
                               : shared.clientList[i].username;
            
            listMsg += "#" + std::to_string(shared.clientList[i].id) + " - " + name;
            if (i < shared.clientList.size() - 1) {
                listMsg += ",";
            }
        }
    }
    listMsg += "\n"; // Explicit packet line ending delimiter

    // Send it out to everyone through your existing broadcast hub
    netManager.broadCasting(listMsg);
}