#include "Thread/networkManagerClient.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <fstream>
#include <vector>

#define CHUNK_SIZE 4096   // send image in 4KB chunks

// ── recvThread ────────────────────────────────────────────────────
void NetworkManagerClient::recvThread()
{
    char buf[512];
    std::string accumulationBuffer = "";

    while (shared_.running && shared_.connected)
    {
        int retval = recv(shared_.sock, buf, sizeof(buf), 0);
        if (retval == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            std::lock_guard<std::mutex> lock(shared_.mtx);
            shared_.messageLog.push_back({"[ERR] Recv error (code " + std::to_string(err) + ")", 0.0});
            break;
        }
        if (retval == 0)
        {
            std::lock_guard<std::mutex> lock(shared_.mtx);
            shared_.messageLog.push_back({"[-] Server closed connection.", 0.0});
            break;
        }

        shared_.byteReceived += retval;

        // Append newly received data into our stream accumulator
        accumulationBuffer.append(buf, retval);

        // Process all complete lines available in the buffer
        size_t newlinePos;
        while ((newlinePos = accumulationBuffer.find('\n')) != std::string::npos)
        {
            std::string raw = accumulationBuffer.substr(0, newlinePos);
            accumulationBuffer.erase(0, newlinePos + 1); // Remove the processed line from buffer

            double ts = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();

            float value = 0.0f;
            try { value = std::stof(raw); }
            catch (...) { value = static_cast<float>(raw.size()); }

            {
                std::lock_guard<std::mutex> lock(shared_.mtx);
                shared_.messageLog.push_back({ "[Server] " + raw, ts });

                // DM received: "[Client #N → You] body"
                if (raw.find("→ You]") != std::string::npos)
                {
                    size_t hash  = raw.find('#');
                    size_t space = raw.find(' ', hash);
                    size_t brace = raw.find("] ", space);

                    if (hash != std::string::npos && space != std::string::npos && brace != std::string::npos) {
                        std::string senderId = raw.substr(hash, space - hash);
                        std::string body   = raw.substr(brace + 2);

                        // Convert sender ID to username for display
                        std::string senderName = senderId;
                        auto it = shared_.idToUsername.find(senderId);
                        if (it != shared_.idToUsername.end()) {
                            senderName = it->second;
                        }

                        shared_.chatHistory[senderName].push_back({ senderName, body, ts, false });
                    }
                }
                // Broadcast received: "[Client #N → ALL] body"
                else if (raw.find("→ ALL]") != std::string::npos)
                {
                    size_t hash  = raw.find('#');
                    size_t space = raw.find(' ', hash);
                    size_t brace = raw.find("] ", space);

                    if (hash != std::string::npos && space != std::string::npos && brace != std::string::npos) {
                        std::string senderId = raw.substr(hash, space - hash);
                        std::string body   = raw.substr(brace + 2);

                        // Convert sender ID to username for display
                        std::string senderName = senderId;
                        auto it = shared_.idToUsername.find(senderId);
                        if (it != shared_.idToUsername.end()) {
                            senderName = it->second;
                        }

                        shared_.chatHistory["ALL"].push_back({ senderName, body, ts, false });
                    }
                }
                // ONLINE_LIST: parse online users and build ID mapping
                else if (raw.substr(0, 12) == "ONLINE_LIST:")
                {
                    std::string listContent = raw.substr(12);
                    shared_.usernameToID.clear();
                    shared_.idToUsername.clear();
                    shared_.onlineUsers.clear();

                    size_t pos = 0;
                    while (pos < listContent.size())
                    {
                        size_t hashPos = listContent.find('#', pos);
                        if (hashPos == std::string::npos) break;

                        size_t dashPos = listContent.find(" - ", hashPos);
                        if (dashPos == std::string::npos) break;

                        size_t commaPos = listContent.find(',', dashPos);
                        if (commaPos == std::string::npos)
                            commaPos = listContent.size();

                        std::string clientId = listContent.substr(hashPos, dashPos - hashPos);
                        std::string username = listContent.substr(dashPos + 3, commaPos - (dashPos + 3));

                        shared_.usernameToID[username] = clientId;
                        shared_.idToUsername[clientId] = username;
                        shared_.onlineUsers.push_back(username);

                        pos = commaPos + 1;
                    }

                    shared_.chatHistory["__LIST__"].push_back({
                        "System",
                        "Online users updated: " + std::to_string(shared_.onlineUsers.size()) + " users",
                        ts, false
                    });
                }
                // IMG ACK or server responses
                else if (raw.find("IMG_ACK:") != std::string::npos || raw.substr(0, 8) == "[Server]")
                {
                    shared_.chatHistory["ALL"].push_back({ "Server", raw, ts, false });
                }
                else
                {
                    shared_.chatHistory["ALL"].push_back({ "Server", raw, ts, false });
                }

                shared_.plotBuffer.push_back(value);
                shared_.newDataReady = true;
            }
        }
    }

    shared_.connected = false;
    if (shared_.sock != INVALID_SOCKET)
    {
        closesocket(shared_.sock);
        shared_.sock = INVALID_SOCKET;
    }

    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[-] Disconnected.", 0.0});
        shared_.chatHistory["ALL"].push_back({
            "System", "Disconnected from server.", 0.0, false
        });
        shared_.newDataReady = true;
    }
}

// ── sendMessage — also writes into chatHistory ────────────────────
void NetworkManagerClient::sendMessage(const std::string& message)
{
    if (!shared_.connected || shared_.sock == INVALID_SOCKET)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Not connected. Cannot send message.", 0.0});
        shared_.newDataReady = true;
        return;
    }

    // Validate message length (prevent buffer overflow)
    if (message.empty() || message.size() > 4096)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        if (message.empty())
            shared_.messageLog.push_back({"[ERR] Cannot send empty message.", 0.0});
        else
            shared_.messageLog.push_back({"[ERR] Message too long (max 4096 bytes).", 0.0});
        shared_.newDataReady = true;
        return;
    }

    // Enforce message ending boundary character
    std::string framedMessage = message + "\n";

    int retval = ::send(shared_.sock,
                        framedMessage.c_str(),
                        static_cast<int>(framedMessage.size()), 0);

    if (retval == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Send failed (code " + std::to_string(err) + ")", 0.0});
        shared_.connected = false;
        shared_.newDataReady = true;
        return;
    }

    shared_.byteSent += retval;

    double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::lock_guard<std::mutex> lock(shared_.mtx);

    // ── Route sent message into correct chat bucket ───────────────
    if (message.substr(0, 7) == "TO:ALL ")
    {
        std::string body = message.substr(7);
        shared_.chatHistory["ALL"].push_back({
            "You", body, ts, true
        });
    }
    else if (message.substr(0, 3) == "TO:")
    {
        // "TO:#2 hello"
        size_t      space  = message.find(' ');
        std::string target = message.substr(3, space - 3);
        std::string body   = message.substr(space + 1);

        std::string displayName = target;
        auto it = shared_.idToUsername.find(target);
        if (it != shared_.idToUsername.end()) {
            displayName = it->second;
        }

        shared_.chatHistory[displayName].push_back({
            "You", body, ts, true
        });
    }
    else
    {
        // Raw message — goes to ALL
        shared_.chatHistory["ALL"].push_back({
            "You", message, ts, true
        });
    }

    shared_.messageLog.push_back({"[You] " + message, ts});
    shared_.newDataReady = true;
}

// ── setUsername — add system message to ALL chat ──────────────────
void NetworkManagerClient::setUsername(const std::string& name)
{
    if (name.empty() || name.size() > 64)
    {
        return;
    }

    shared_.username = name;

    if (shared_.connected && shared_.sock != INVALID_SOCKET)
    {
        std::string msg = "NAME:" + name + "\n";
        ::send(shared_.sock, msg.c_str(),
               static_cast<int>(msg.size()), 0);
    }

    double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::lock_guard<std::mutex> lock(shared_.mtx);
    shared_.chatHistory["ALL"].push_back({
        "System",
        "Username set to: " + name,
        ts, false
    });
    shared_.newDataReady = true;
}

// ── sendWho ───────────────────────────────────────────────────────
void NetworkManagerClient::sendWho()
{
    double ts = std::chrono::duration<double>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.chatHistory["ALL"].push_back({
            "You",
            shared_.username + " (this client)",
            ts, true
        });
        shared_.newDataReady = true;
    }

    if (shared_.connected && shared_.sock != INVALID_SOCKET)
        sendMessage("WHO");
}

// ── connect ───────────────────────────────────────────────────────
void NetworkManagerClient::connect(const std::string& ip, int port)
{
    if (shared_.connected)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[~] Already connected. Disconnect first.", 0.0});
        shared_.newDataReady = true;
        return;
    }

    if (port <= 0 || port > 65535)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Invalid port: " + std::to_string(port), 0.0});
        shared_.newDataReady = true;
        throw std::runtime_error("Invalid port");
    }

    shared_.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (shared_.sock == INVALID_SOCKET)
    {
        int err = WSAGetLastError();
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Socket creation failed (code " + std::to_string(err) + ")", 0.0});
        shared_.newDataReady = true;
        throw std::runtime_error("socket() failed: " + std::to_string(err));
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) != 1)
    {
        closesocket(shared_.sock);
        shared_.sock = INVALID_SOCKET;
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Invalid IP address: " + ip, 0.0});
        shared_.newDataReady = true;
        throw std::runtime_error("Invalid IP: " + ip);
    }

    if (::connect(shared_.sock,
                  reinterpret_cast<sockaddr*>(&serverAddr),
                  sizeof(serverAddr)) == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        closesocket(shared_.sock);
        shared_.sock = INVALID_SOCKET;
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Connection failed to " + ip + ":" + std::to_string(port) + " (code " + std::to_string(err) + ")", 0.0});
        shared_.newDataReady = true;
        throw std::runtime_error("connect() failed: " + std::to_string(err));
    }

    shared_.connected    = true;
    shared_.byteReceived = 0;
    shared_.byteSent     = 0;

    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({
            "[+] Connected to " + ip + ":" + std::to_string(port), 0.0
        });
        shared_.newDataReady = true;
    }

    if (recv_thread_.joinable()) recv_thread_.join();
    recv_thread_ = std::thread(&NetworkManagerClient::recvThread, this);

    // Handshake: send identity and request online user list
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string identityMsg = "NAME:" + shared_.username + "\n";
    ::send(shared_.sock, identityMsg.c_str(), static_cast<int>(identityMsg.size()), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sendMessage("WHO");
}

// ── disconnect ────────────────────────────────────────────────────
void NetworkManagerClient::disconnect()
{
    if (!shared_.connected && !recv_thread_.joinable()) return;

    shared_.running   = false;
    shared_.connected = false;

    if (shared_.sock != INVALID_SOCKET)
    {
        closesocket(shared_.sock);
        shared_.sock = INVALID_SOCKET;
    }

    if (recv_thread_.joinable())       recv_thread_.join();
    if (send_image_thread_.joinable()) send_image_thread_.join();

    shared_.running = true;
}


// ── sendImage (spawns background thread) ─────────────────────────
void NetworkManagerClient::sendImage(const std::string& filepath)
{
    if (!shared_.connected)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Not connected.", 0.0});
        shared_.newDataReady = true;
        return;
    }

    if (shared_.sendingImage)
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Already sending an image.", 0.0});
        shared_.newDataReady = true;
        return;
    }

    if (send_image_thread_.joinable())
        send_image_thread_.join();

    send_image_thread_ = std::thread(
        &NetworkManagerClient::sendImageThread, this, filepath);
}

// ── sendImageThread ───────────────────────────────────────────────
void NetworkManagerClient::sendImageThread(const std::string& filepath)
{
    shared_.sendingImage    = true;
    shared_.imgSendProgress = 0;

    // ── Open file ─────────────────────────────────────────────────
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Cannot open file: " + filepath, 0.0});
        shared_.newDataReady = true;
        shared_.sendingImage = false;
        return;
    }

    // ── Read entire file into buffer ──────────────────────────────
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> fileData(fileSize);
    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize))
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Failed to read file.", 0.0});
        shared_.newDataReady = true;
        shared_.sendingImage = false;
        return;
    }
    file.close();

    // ── Extract filename from path ────────────────────────────────
    std::string filename = filepath;
    size_t slash = filepath.find_last_of("/\\");
    if (slash != std::string::npos)
        filename = filepath.substr(slash + 1);

    // ── Send header: "IMG:<filename>:<size>\n" ────────────────────
    std::string header = "IMG:" + filename
                       + ":" + std::to_string(fileSize) + "\n";

    int sent = ::send(
        shared_.sock,
        header.c_str(),
        static_cast<int>(header.size()), 0
    );

    if (sent == SOCKET_ERROR)
    {
        int err = WSAGetLastError();
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({"[ERR] Failed to send image header (code " + std::to_string(err) + ")", 0.0});
        shared_.connected = false;
        shared_.newDataReady = true;
        shared_.sendingImage = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        shared_.messageLog.push_back({
            "[IMG] Sending: " + filename
            + " (" + std::to_string(fileSize) + " bytes)", 0.0
        });
        shared_.newDataReady = true;
    }

    // ── Send file in chunks ───────────────────────────────────────
    size_t totalSent = 0;

    while (totalSent < fileData.size() && shared_.connected)
    {
        size_t remaining  = fileData.size() - totalSent;
        size_t chunkSize  = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;

        int result = ::send(
            shared_.sock,
            reinterpret_cast<const char*>(fileData.data() + totalSent),
            static_cast<int>(chunkSize), 0
        );

        if (result == SOCKET_ERROR)
        {
            int err = WSAGetLastError();
            std::lock_guard<std::mutex> lock(shared_.mtx);
            shared_.messageLog.push_back({"[ERR] Image send interrupted (code " + std::to_string(err) + ")", 0.0});
            shared_.connected = false;
            shared_.newDataReady = true;
            break;
        }

        totalSent += result;
        shared_.byteSent += result;

        // Update progress 0-100
        shared_.imgSendProgress = static_cast<int>(
            (totalSent * 100) / fileData.size()
        );
    }

    // ── Done ──────────────────────────────────────────────────────
    {
        std::lock_guard<std::mutex> lock(shared_.mtx);
        if (totalSent == fileData.size())
        {
            double ts = std::chrono::duration<double>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count();
            shared_.chatHistory["ALL"].push_back({
                "System",
                "[+] File sent: " + filename,
                ts, false
            });
            shared_.messageLog.push_back({
                "[IMG] Sent successfully: " + filename, 0.0
            });
        }
        shared_.newDataReady = true;
    }

    shared_.imgSendProgress = 100;
    shared_.sendingImage    = false;

    printf("[sendImageThread] Done. Sent %zu / %lld bytes.\n",
           totalSent, (long long)fileSize);
}