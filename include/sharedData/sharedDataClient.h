#pragma once

#include <mutex>
#include <vector>
#include <string>
#include <atomic>
#include <map>
#include <winsock2.h>

struct ReceivedMessage
{
    std::string text;
    double      timestamp;
};

struct ChatMessage
{
    std::string sender;     // "You", "#2", "Server", "System"
    std::string body;       // actual message text
    double      timestamp;
    bool        isOwn;      // true = right-align in chat
    bool        isImage     =   false;    // true if this message represents an image (for future use)
    std::string imagePath   =   ""; // if isImage is true, this holds the filename of the image
};

struct SharedDataClient
{
    std::mutex                               mtx;
    std::vector<ReceivedMessage>             messageLog;
    std::vector<float>                       plotBuffer;    // timestamps and values of received packets for plotting
    std::map<std::string,
             std::vector<ChatMessage>>       chatHistory;   // key = "ALL", "#2", etc.
    std::vector<std::string>                 onlineUsers;   // List of usernames currently online
    std::map<std::string, std::string>       usernameToID;  // username → "#N"
    std::map<std::string, std::string>       idToUsername;  // "#N" → username
    std::atomic<bool>                        running        { true  };
    std::atomic<bool>                        newDataReady   { false };
    std::atomic<bool>                        connected      { false };
    std::atomic<bool>                        sendingImage   { false };
    std::atomic<int>                         byteReceived   { 0     };
    std::atomic<int>                         byteSent       { 0     };
    std::atomic<int>                         imgSendProgress{ 0     };
    std::string                              username       = "Client";
    SOCKET                                   sock           = INVALID_SOCKET;
};