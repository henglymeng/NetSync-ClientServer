# Network Communication System with GUI Monitoring

## Overview

This project is a multithreaded client-server communication system developed in C++. It combines networking, concurrent programming, graphical user interfaces, and real-time data visualization.

The application allows multiple clients to communicate with a server while displaying network activity and performance statistics through an interactive GUI.

## Features

* TCP/IP client-server communication
* Multithreaded network processing
* Thread-safe data synchronization
* Interactive GUI using ImGui
* Real-time graph plotting using Matplot++
* Connection management for multiple clients
* Modular architecture for scalability and maintenance

---

## Technologies Used

| Technology                   | Purpose                                       |
| ---------------------------- | --------------------------------------------- |
| C++17/20                     | Core application development                  |
| GLFW                         | Window creation and OpenGL context management |
| ImGui                        | Graphical user interface                      |
| Matplot++                    | Real-time graph plotting                      |
| Winsock / Socket API         | Network communication                         |
| CMake                        | Cross-platform build system                   |
| Multithreading (std::thread) | Concurrent client/server processing           |

---

## System Architecture

### Server Side

The server consists of three main components:

#### Accept Thread

Responsible for listening for incoming client connections and creating worker threads.

#### Worker Thread

Handles communication with individual clients.

#### Network Manager

Coordinates active connections and shared resources.

### Client Side

#### Network Manager Client

Maintains communication with the server and processes incoming data.

#### GUI Application

Displays network information, statistics, and graphical visualizations.

---

## Project Structure

```text
.
├── external/
│   ├── glfw/
│   ├── imgui/
│   └── matplotplusplus/
│
├── include/
│   ├── glad/
│   ├── GLFW/
│   ├── imgui/
│   ├── sharedData/
│   └── Thread/
│
├── src/
│   ├── Client/
│   │   ├── Thread/
│   │   └── NetAppClient.cpp
│   │
│   └── Server/
│       ├── Thread/
│       └── NetAppServer.cpp
│
├── CMakeLists.txt
└── README.md
```

---

## Build Instructions

### Prerequisites

* CMake 3.16+
* C++17 compatible compiler
* OpenGL support

### Build

```bash
mkdir build
cd build

cmake ..
cmake --build .
```

### Run

Start the server:

```bash
./NetAppServer
```

Start the client:

```bash
./NetAppClient
```

---

## Threading Model

The application uses multiple threads to separate responsibilities:

* Main Thread: GUI rendering
* Accept Thread: Incoming connection handling
* Worker Threads: Client communication
* Network Manager Thread: Message coordination

This design prevents GUI blocking and improves responsiveness during network operations.

---

## Data Flow

```text
Client
   │
   ▼
Network Manager Client
   │
   ▼
TCP Socket
   │
   ▼
Server Accept Thread
   │
   ▼
Worker Thread
   │
   ▼
Shared Data
   │
   ▼
GUI + Real-Time Graph
```

---

## Learning Objectives

This project demonstrates:

* Socket programming
* Multithreaded application design
* Synchronization of shared resources
* GUI development with ImGui
* Real-time data visualization
* Modular software architecture

---

## Future Improvements

* Support for multiple simultaneous clients
* Performance monitoring dashboard
* Message logging system
* Configuration file support
* Cross-platform networking abstraction

---

## Author

Meng

Engineering Student
