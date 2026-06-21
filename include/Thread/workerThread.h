#pragma once

#include <winsock2.h>
#include "sharedData/sharedDataServer.h"

void broadcastOnlineList(SharedData& shared);
void workerThread(SOCKET sock, sockaddr_in clientAddr, int clientID, SharedData& shared);