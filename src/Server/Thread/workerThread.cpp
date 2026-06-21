#include "Thread/workerThread.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <fstream>

#define BUFSIZE 512

static bool recvAll(SOCKET sock, uint8_t *buf, size_t size,
                    std::atomic<bool> &running)
{
    size_t received = 0;
    while (received < size && running)
    {
        int r = recv(sock,
                     reinterpret_cast<char *>(buf + received),
                     static_cast<int>(size - received), 0);
        if (r <= 0)
            return false;
        received += r;
    }
    return received == size;
}

// ── Forward message to a specific client by ID ────────────────────
static void forwardTo(int targetID, int senderID,
                      const std::string &message,
                      SharedData &shared)
{
    // Appends internal system formatting + the required packet boundary delimiter
    std::string payload = "[Client #" + std::to_string(senderID) + " -> You] " + message + "\n";

    for (const auto &c : shared.clientList)
    {
        if (c.id == targetID)
        {
            int retval = ::send(c.sock, payload.c_str(),
                                static_cast<int>(payload.size()), 0);
            if (retval == SOCKET_ERROR)
            {
                int err = WSAGetLastError();
                std::lock_guard<std::mutex> lock(shared.mtx);
                shared.messageLog.push_back(
                    "[ERR] Forward to #" + std::to_string(targetID) +
                    " failed (code " + std::to_string(err) + ")");
            }
            return;
        }
    }
}

// Full-send helper to ensure all bytes are sent (handles partial sends)
static bool sendAll(SOCKET sock, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        int r = ::send(sock, data + sent, static_cast<int>(len - sent), 0);
        if (r == SOCKET_ERROR)
            return false;
        sent += r;
    }
    return true;
}

// image-forwarding helper function to send image data to a specific client
static void forwardImageTo(int targetID, int senderID,
                           const std::string &filename,
                           const std::vector<uint8_t> &data,
                           const std::string &scope,
                           SharedData &shared)
{
    std::string header = "IMG:#" + std::to_string(senderID) + ":" + scope + ":" + filename + ":" + std::to_string(data.size()) + "\n";

    for (const auto &c : shared.clientList)
    {
        if (c.id != targetID)
            continue;
        if (!sendAll(c.sock, header.c_str(), header.size()) ||
            !sendAll(c.sock, reinterpret_cast<const char *>(data.data()), data.size()))
        {
            shared.messageLog.push_back(
                "[ERR] Image forward to #" + std::to_string(targetID) + " failed");
        }
        return;
    }
}

// ── Build WHO response ─────────────────────────────────────────────
static std::string buildWhoResponse(SharedData &shared)
{
    std::string resp = "[Server] Connected clients:\n";
    for (const auto &c : shared.clientList)
    {
        std::string name = c.username.empty()
                               ? "Client"
                               : c.username;
        resp += "  #" + std::to_string(c.id) + " " + name + " (" + c.ip + ":" + std::to_string(c.port) + ")\n";
    }
    return resp;
}

void broadcastOnlineList(SharedData& shared)
{
    std::vector<ClientInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(shared.mtx);
        snapshot = shared.clientList;
    }

    std::string listMsg = "ONLINE_LIST:";
    for (size_t i = 0; i < snapshot.size(); ++i)
    {
        const auto& c = snapshot[i];
        std::string name = c.username.empty() ? "Client" + std::to_string(c.id) : c.username;
        listMsg += "#" + std::to_string(c.id) + " - " + name;
        if (i + 1 < snapshot.size()) listMsg += ",";
    }
    listMsg += "\n";

    for (const auto& c : snapshot)
        ::send(c.sock, listMsg.c_str(), static_cast<int>(listMsg.size()), 0);
}

void workerThread(SOCKET sock, sockaddr_in clientAddr,
                  int clientID, SharedData &shared)
{
    char addrStr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
    int port = ntohs(clientAddr.sin_port);
    std::string tag = "#" + std::to_string(clientID) + " " + std::string(addrStr) + ":" + std::to_string(port);

    char buf[BUFSIZE];
    std::string accumulationBuffer = "";

    while (shared.running)
    {
        int retval = recv(sock, buf, sizeof(buf), 0);
        if (retval == SOCKET_ERROR || retval == 0)
            break;

        shared.packetAccum.fetch_add(1, std::memory_order_relaxed);
        accumulationBuffer.append(buf, retval);

        // Process bytes out of our stream buffer
        size_t newlinePos;
        while ((newlinePos = accumulationBuffer.find('\n')) != std::string::npos)
        {
            std::string msg = accumulationBuffer.substr(0, newlinePos);

            // ── CASE 1: IMAGE PAYLOAD HANDOFF ────────────────────────────────
            if (msg.substr(0, 4) == "IMG:")
            {
                // New format: IMG:<target>:<filename>:<size>
                size_t c1 = msg.find(':', 4);
                size_t c2 = (c1 == std::string::npos) ? std::string::npos : msg.find(':', c1 + 1);
                size_t headerTotalLength = newlinePos + 1;

                if (c1 == std::string::npos || c2 == std::string::npos)
                {
                    accumulationBuffer.erase(0, headerTotalLength);
                    continue;
                }

                std::string target = msg.substr(4, c1 - 4);
                std::string filename = msg.substr(c1 + 1, c2 - c1 - 1);

                size_t fileSize = 0;
                try
                {
                    fileSize = std::stoull(msg.substr(c2 + 1));
                }
                catch (...)
                {
                    accumulationBuffer.erase(0, headerTotalLength);
                    continue;
                }

                const size_t MAX_FILE_SIZE = 50 * 1024 * 1024;
                if (fileSize > MAX_FILE_SIZE)
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.messageLog.push_back("[ERR] File too large from #" + std::to_string(clientID));
                    shared.newDataReady = true;
                    accumulationBuffer.erase(0, headerTotalLength);
                    continue;
                }
                if (accumulationBuffer.size() < headerTotalLength + fileSize)
                    break; // wait for more bytes

                const uint8_t *rawPtr = reinterpret_cast<const uint8_t *>(accumulationBuffer.data() + headerTotalLength);
                std::vector<uint8_t> fileData(rawPtr, rawPtr + fileSize);
                accumulationBuffer.erase(0, headerTotalLength + fileSize);

                // ── Relay ──────────────────────────────────────────────
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);

                    if (target == "ALL")
                    {
                        for (const auto &c : shared.clientList)
                            if (c.id != clientID)
                                forwardImageTo(c.id, clientID, filename, fileData, "ALL", shared);

                        shared.messageLog.push_back("[IMG] #" + std::to_string(clientID) +
                                                    " -> ALL: " + filename + " (" + std::to_string(fileSize) + " bytes)");
                    }
                    else if (!target.empty() && target[0] == '#')
                    {
                        try
                        {
                            int targetID = std::stoi(target.substr(1));
                            forwardImageTo(targetID, clientID, filename, fileData, "DM", shared);
                            shared.messageLog.push_back("[IMG] #" + std::to_string(clientID) +
                                                        " -> #" + std::to_string(targetID) + ": " + filename);
                        }
                        catch (...)
                        {
                        }
                    }

                    for (auto &c : shared.clientList)
                        if (c.sock == sock)
                        {
                            c.imgCount++;
                            break;
                        }

                    shared.newDataReady = true;
                }

                // ACK back to sender
                std::string ack = "IMG_ACK:" + filename + "\n";
                sendAll(sock, ack.c_str(), ack.size());

                continue;
            }

            // ── CASE 2: TEXT MESSAGE / STRUCTURAL CONTROL PACKETS ────────────
            accumulationBuffer.erase(0, newlinePos + 1); // Safely pop text command out of the line buffer

            // ── NAME:<username> ───────────────────────────────────────
            if (msg.substr(0, 5) == "NAME:")
            {
                std::string newName = msg.substr(5);
                newName.erase(0, newName.find_first_not_of(" \t\r\n"));
                newName.erase(newName.find_last_not_of(" \t\r\n") + 1);

                if (!newName.empty())
                {
                    {
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        for (auto &c : shared.clientList)
                        {
                            if (c.sock == sock)
                            {
                                std::string old = c.username;
                                c.username = newName;
                                shared.messageLog.push_back(
                                    "[~] Client #" + std::to_string(clientID) + " renamed: " + old + " -> " + newName);
                                break;
                            }
                        }
                        shared.newDataReady = true;
                    }
                    broadcastOnlineList(shared); 
                }
                continue;
            }

            // ── WHO — list connected clients ──────────────────────────
            if (msg == "WHO")
            {
                std::string resp;
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    resp = buildWhoResponse(shared);
                }
                // Pre-formatted with its own trailing \n from buildWhoResponse
                int ret = ::send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
                if (ret == SOCKET_ERROR)
                {
                    int err = WSAGetLastError();
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.messageLog.push_back(
                        "[ERR] Failed to send WHO response (code " + std::to_string(err) + ")");
                }
                continue;
            }

            // ── TO:#N <message> — direct message parsing ──────────────
            if (msg.substr(0, 3) == "TO:")
            {
                size_t space = msg.find(' ');
                if (space == std::string::npos)
                {
                    std::string err = "[Server] Usage: TO:#<id> <message>\n";
                    int ret = ::send(sock, err.c_str(), static_cast<int>(err.size()), 0);
                    if (ret == SOCKET_ERROR)
                    {
                        int errno_val = WSAGetLastError();
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        shared.messageLog.push_back(
                            "[ERR] Failed to send error message (code " + std::to_string(errno_val) + ")");
                    }
                    continue;
                }

                std::string target = msg.substr(3, space - 3);
                std::string payload = msg.substr(space + 1);

                if (target == "ALL")
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    std::string fwd = "[Client #" + std::to_string(clientID) + " -> ALL] " + payload + "\n";

                    for (const auto &c : shared.clientList)
                    {
                        if (c.id == clientID)
                            continue;
                        int ret = ::send(c.sock, fwd.c_str(), static_cast<int>(fwd.size()), 0);
                        if (ret == SOCKET_ERROR)
                        {
                            int err = WSAGetLastError();
                            shared.messageLog.push_back(
                                "[ERR] Failed to relay to #" + std::to_string(c.id) +
                                " (code " + std::to_string(err) + ")");
                        }
                    }

                    shared.messageLog.push_back("[RELAY ALL] #" + std::to_string(clientID) + " -> ALL: " + payload);
                    shared.newDataReady = true;
                }
                else if (target[0] == '#')
                {
                    try
                    {
                        int targetID = std::stoi(target.substr(1));
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        forwardTo(targetID, clientID, payload, shared);

                        shared.messageLog.push_back("[RELAY] #" + std::to_string(clientID) + " -> #" + std::to_string(targetID) + ": " + payload);
                        shared.newDataReady = true;
                    }
                    catch (...)
                    {
                        std::string err = "[Server] Invalid Target ID format.\n";
                        ::send(sock, err.c_str(), static_cast<int>(err.size()), 0);
                    }
                }
                else
                {
                    std::string err = "[Server] Unknown target: " + target + "\n";
                    ::send(sock, err.c_str(), static_cast<int>(err.size()), 0);
                }
                continue;
            }

            // ── Normal message — Broadcast to ALL + log ──────────────────────
            double ts = std::chrono::duration<double>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

            double value = 0.0;
            try
            {
                value = std::stod(msg);
            }
            catch (...)
            {
                value = static_cast<double>(msg.size());
            }

            {
                std::lock_guard<std::mutex> lock(shared.mtx);
                shared.plotBuffer.push_back({ts, value, clientID});

                // Get the sender's current username or fall back to default tag
                std::string senderName = "Client #" + std::to_string(clientID);
                for (const auto &c : shared.clientList)
                {
                    if (c.sock == sock && !c.username.empty())
                    {
                        senderName = c.username;
                        break;
                    }
                }

                // Format it so the client's "-> ALL]" parser catches it flawlessly
                std::string broadcastPayload = "[" + senderName + " -> ALL] " + msg + "\n";

                // Loop through and relay it to ALL connected clients (including/excluding sender as preferred)
                for (const auto &c : shared.clientList)
                {
                    // If you want to echo back to sender too, remove the 'if' guard line below
                    if (c.id == clientID)
                        continue;
                    ::send(c.sock, broadcastPayload.c_str(), static_cast<int>(broadcastPayload.size()), 0);
                }

                shared.messageLog.push_back("[BROADCAST] " + senderName + ": " + msg);
                for (auto &c : shared.clientList)
                    if (c.sock == sock)
                    {
                        c.msgCount++;
                        break;
                    }

                shared.newDataReady = true;
            }

            std::string echoStr = msg + "\n";
            ::send(sock, echoStr.c_str(), static_cast<int>(echoStr.size()), 0);
        }
    }

    // ── Disconnect cleanup ────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(shared.mtx);
        shared.clientList.erase(
            std::remove_if(
                shared.clientList.begin(),
                shared.clientList.end(),
                [sock](const ClientInfo &c)
                { return c.sock == sock; }),
            shared.clientList.end());
        shared.messageLog.push_back("[-] Client " + tag + " disconnected");
        shared.newDataReady = true;
    }

    broadcastOnlineList(shared);

    shared.clientCount--;
    closesocket(sock);
}