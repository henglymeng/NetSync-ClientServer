#pragma once

#include <winsock2.h>
#include "sharedData/sharedDataServer.h"

void acceptThread(SOCKET listen_sock, SharedData& shared);