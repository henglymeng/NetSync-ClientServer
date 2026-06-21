#include "Thread/acceptThread.h"
#include "Thread/workerThread.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <thread>
#include <string>

void acceptThread(SOCKET listen_sock, SharedData& shared)
{
    printf("[acceptThread] Waiting for connections...\n");

    while (shared.running)
    {
        sockaddr_in clientAddr = {};
        int         addrLen    = sizeof(clientAddr);

        SOCKET client_sock = accept(
            listen_sock,
            reinterpret_cast<sockaddr*>(&clientAddr),
            &addrLen
        );

        if (!shared.running) break;

        if (client_sock == INVALID_SOCKET)
        {
            if (!shared.running)
                break;

            fprintf(stderr,
                    "[acceptThread] accept() failed: %d\n",
                    WSAGetLastError());

            continue;
        }

        // ── Build ClientInfo ──────────────────────────────────────
        char addrStr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &clientAddr.sin_addr, addrStr, sizeof(addrStr));
        int port = ntohs(clientAddr.sin_port);

        // Assign unique ID
        int id = shared.nextClientID.fetch_add(1);

        ClientInfo info {
            std::string(addrStr),
            port,
            client_sock,
            id,
            0,
            0,
            "Client" + std::to_string(id) // default username
        };

        printf("[acceptThread] Client #%d connected: %s:%d\n",
               id, addrStr, port);

        // ── Register in shared state ──────────────────────────────
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            shared.clientList.push_back(info);
            shared.messageLog.push_back(
                "[+] Client #" + std::to_string(id)
                + " connected: " + std::string(addrStr)
                + ":" + std::to_string(port)
            );
            shared.newDataReady = true;
        }

        broadcastOnlineList(shared);   // tell everyone a new client joined

        shared.clientCount.fetch_add(1, std::memory_order_relaxed);

        // ── Spawn dedicated worker ────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            shared.workerThreads.emplace_back(workerThread, client_sock, clientAddr, id,
                                              std::ref(shared));
        }
    }

    printf("[acceptThread] Exiting.\n");
}