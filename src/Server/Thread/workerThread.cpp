#include "Thread/workerThread.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>

#define BUFSIZE 512

static bool recvAll(SOCKET sock, uint8_t* buf, size_t size,
                    std::atomic<bool>& running)
{
    size_t received = 0;
    while (received < size && running)
    {
        int r = recv(sock,
                     reinterpret_cast<char*>(buf + received),
                     static_cast<int>(size - received), 0);
        if (r <= 0) return false;
        received += r;
    }
    return received == size;
}

// ── Forward message to a specific client by ID ────────────────────
static void forwardTo(int targetID, int senderID,
                      const std::string& message,
                      SharedData& shared)
{
    // Appends internal system formatting + the required packet boundary delimiter
    std::string payload = "[Client #" + std::to_string(senderID)
                        + " -> You] " + message + "\n";

    for (const auto& c : shared.clientList)
    {
        if (c.id == targetID)
        {
            ::send(c.sock, payload.c_str(),
                   static_cast<int>(payload.size()), 0);
            return;
        }
    }
}

// ── Build WHO response ─────────────────────────────────────────────
static std::string buildWhoResponse(SharedData& shared)
{
    std::string resp = "[Server] Connected clients:\n";
    for (const auto& c : shared.clientList)
    {
        std::string name = c.username.empty()
                         ? "Client" : c.username;
        resp += "  #" + std::to_string(c.id)
              + " " + name
              + " (" + c.ip
              + ":" + std::to_string(c.port) + ")\n";
    }
    return resp;
}

void workerThread(SOCKET sock, sockaddr_in clientAddr,
                  int clientID, SharedData& shared)
{
    char addrStr[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
    int port = ntohs(clientAddr.sin_port);
    std::string tag = "#" + std::to_string(clientID)
                    + " " + std::string(addrStr)
                    + ":" + std::to_string(port);

    char buf[BUFSIZE];
    std::string accumulationBuffer = ""; 

    while (shared.running)
    {
        int retval = recv(sock, buf, sizeof(buf), 0);
        if (retval == SOCKET_ERROR || retval == 0) break;

        shared.packetAccum++;
        accumulationBuffer.append(buf, retval);

        // Process bytes out of our stream buffer
        size_t newlinePos;
        while ((newlinePos = accumulationBuffer.find('\n')) != std::string::npos)
        {
            std::string msg = accumulationBuffer.substr(0, newlinePos);

            // ── CASE 1: IMAGE PAYLOAD HANDOFF ────────────────────────────────
            if (msg.substr(0, 4) == "IMG:")
            {
                // msg string format: "IMG:filename.ext:size"
                size_t firstColon  = msg.find(':', 4);          // Finds separator after filename
                size_t headerTotalLength = newlinePos + 1;      // Total bytes including '\n'

                if (firstColon == std::string::npos)
                {
                    // Malformed protocol header frame, clear it out to prevent infinite loops
                    accumulationBuffer.erase(0, headerTotalLength);
                    continue;
                }

                std::string filename = msg.substr(4, firstColon - 4);
                
                size_t fileSize = 0;
                try {
                    fileSize = std::stoull(msg.substr(firstColon + 1));
                }
                catch (...) {
                    // Size parsing failed
                    accumulationBuffer.erase(0, headerTotalLength);
                    continue;
                }

                // Check if the full binary segment has reached our user-space stream buffer yet
                if (accumulationBuffer.size() < headerTotalLength + fileSize)
                {
                    // Fragmentation hit! The stream is missing bytes.
                    // Break out of processing and let the main recv loop pull more data.
                    break; 
                }

                // Extract binary payload out of stream string container
                const uint8_t* rawBinaryDataPtr = reinterpret_cast<const uint8_t*>(accumulationBuffer.data() + headerTotalLength);
                std::vector<uint8_t> fileData(rawBinaryDataPtr, rawBinaryDataPtr + fileSize);

                // Process/Save Binary File Data ──────────────────
                std::filesystem::create_directories("Images");

                std::ofstream outfile(
                    std::filesystem::path("Images") / filename, 
                    std::ios::binary);

                outfile.write(
                    reinterpret_cast<const char*>(fileData.data()), 
                    static_cast<std::streamsize>(fileSize)
                );
                
                outfile.close();

                // Drop both text header and binary block from the stream buffer entirely
                accumulationBuffer.erase(0, headerTotalLength + fileSize);

                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.messageLog.push_back(
                        "[IMG] Received file from #" + std::to_string(clientID) + 
                        ": " + filename + " (" + std::to_string(fileSize) + " bytes)"
                    );
                    shared.newDataReady = true;
                }

                // Send back protocol verification ACK
                std::string ack = "IMG_ACK:" + filename + "\n";
                ::send(sock, ack.c_str(), static_cast<int>(ack.size()), 0);

                continue; // Run the loop check again on remaining buffer elements
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
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    for (auto& c : shared.clientList)
                    {
                        if (c.sock == sock)
                        {
                            std::string old = c.username;
                            c.username = newName;
                            shared.messageLog.push_back(
                                "[~] Client #" + std::to_string(clientID)
                                + " renamed: " + old + " -> " + newName
                            );
                            break;
                        }
                    }
                    shared.newDataReady = true;
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
                ::send(sock, resp.c_str(), static_cast<int>(resp.size()), 0);
                continue;
            }

            // ── TO:#N <message> — direct message parsing ──────────────
            if (msg.substr(0, 3) == "TO:")
            {
                size_t space = msg.find(' ');
                if (space == std::string::npos)
                {
                    std::string err = "[Server] Usage: TO:#<id> <message>\n";
                    ::send(sock, err.c_str(), static_cast<int>(err.size()), 0);
                    continue;
                }

                std::string target  = msg.substr(3, space - 3); 
                std::string payload = msg.substr(space + 1);    

                if (target == "ALL")
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    std::string fwd = "[Client #" + std::to_string(clientID) + " -> ALL] " + payload + "\n";

                    for (const auto& c : shared.clientList)
                    {
                        if (c.id == clientID) continue; 
                        ::send(c.sock, fwd.c_str(), static_cast<int>(fwd.size()), 0);
                    }

                    shared.messageLog.push_back("[RELAY ALL] #" + std::to_string(clientID) + " -> ALL: " + payload);
                    shared.newDataReady = true;
                }
                else if (target[0] == '#')
                {
                    try {
                        int targetID = std::stoi(target.substr(1));
                        std::lock_guard<std::mutex> lock(shared.mtx);
                        forwardTo(targetID, clientID, payload, shared);

                        shared.messageLog.push_back("[RELAY] #" + std::to_string(clientID) + " -> #" + std::to_string(targetID) + ": " + payload);
                        shared.newDataReady = true;
                    }
                    catch(...) {
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
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            double value = 0.0;
            try   { value = std::stod(msg); }
            catch (...) { value = static_cast<double>(msg.size()); }

            {
                std::lock_guard<std::mutex> lock(shared.mtx);
                shared.plotBuffer.push_back({ ts, value, clientID });
                
                // Get the sender's current username or fall back to default tag
                std::string senderName = "Client #" + std::to_string(clientID);
                for (const auto& c : shared.clientList) {
                    if (c.sock == sock && !c.username.empty()) {
                        senderName = c.username;
                        break;
                    }
                }

                // Format it so the client's "-> ALL]" parser catches it flawlessly
                std::string broadcastPayload = "[" + senderName + " -> ALL] " + msg + "\n";

                // Loop through and relay it to ALL connected clients (including/excluding sender as preferred)
                for (const auto& c : shared.clientList)
                {
                    // If you want to echo back to sender too, remove the 'if' guard line below
                    if (c.id == clientID) continue; 
                    ::send(c.sock, broadcastPayload.c_str(), static_cast<int>(broadcastPayload.size()), 0);
                }

                shared.messageLog.push_back("[BROADCAST] " + senderName + ": " + msg);
                for (auto& c : shared.clientList)
                    if (c.sock == sock) { c.msgCount++; break; }
                
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
                [sock](const ClientInfo& c){ return c.sock == sock; }
            ),
            shared.clientList.end()
        );
        shared.messageLog.push_back("[-] Client " + tag + " disconnected");
        shared.newDataReady = true;
    }

    shared.clientCount--;
    closesocket(sock);
}