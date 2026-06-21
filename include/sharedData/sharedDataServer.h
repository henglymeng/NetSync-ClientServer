#pragma once

#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <winsock2.h>

struct ClientInfo
{
    std::string  ip;
    int          port;
    SOCKET       sock;
    int          id;
    int          msgCount; // Number of messages received from this client
    int imgCount; // Number of images received from this client 
    std::string       username;  
};

struct ReceivedImage
{
    int                     clientID;   // "ip:port"
    std::string             filename;
    std::vector<uint8_t>    data;       // raw bytes of the message (could be text or image)
};

struct DataPoint
{
    double      timestamp;
    double      value;
    int clientID;   // "ip:port"
};

struct SharedData
{
    std::mutex               mtx;
    std::vector<ClientInfo>  clientList;
    std::vector<std::string> messageLog;
    std::vector<DataPoint>   plotBuffer;
    std::vector<ReceivedImage> imageList;   // ← add this
    std::vector<std::thread> workerThreads;  // Track all worker threads
    std::atomic<bool>        running            { false };
    std::atomic<bool>        newDataReady       { false };
    std::atomic<int>         clientCount        { 0     };
    std::atomic<int>         nextClientID       { 1     };
    std::atomic<int>         packetPerSecond    { 0     };
    std::atomic<int>         packetAccum        { 0     };   // raw counter
};