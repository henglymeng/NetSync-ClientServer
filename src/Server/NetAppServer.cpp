#include "glad/glad.h"
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui_internal.h"

#include <iostream>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>

#include <winsock2.h>
#include <ws2tcpip.h>

#include "sharedData/sharedDataServer.h"
#include "Thread/acceptThread.h"
#include "Thread/workerThread.h"
#include "Thread/networkManagerServer.h"

using namespace std;

#define SERVERPORT 9000

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void GUI(SharedData& shared, NetworkManager& networkManager);
void RenderUI(SharedData& shared, NetworkManager& networkManager);

int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SharedData shared;

    NetworkManager networkManager(shared, SERVERPORT);

    GUI(shared, networkManager);

    networkManager.stop();
    WSACleanup();
    return 0;
}

// ─────────────────────────────────────────────────────────────────
void GUI(SharedData& shared, NetworkManager& networkManager)
{
    if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // ── Monitor work area ─────────────────────────────────────────
    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    int monX, monY, monW, monH;
    glfwGetMonitorWorkarea(monitor, &monX, &monY, &monW, &monH);

    int winW = static_cast<int>(monW * 0.85f);
    int winH = static_cast<int>(monH * 0.85f);
    int winX = monX + (monW - winW) / 2;
    int winY = monY + (monH - winH) / 2;

    GLFWwindow* window = glfwCreateWindow(
        winW, winH, "NetSync - Server", nullptr, nullptr);
    if (!window) { glfwTerminate(); return; }

    glfwSetWindowPos(window, winX, winY);
    glfwSetWindowSizeLimits(window,
        800, 600,
        GLFW_DONT_CARE, GLFW_DONT_CARE
    );

    // ── Boundary clamp callback ───────────────────────────────────
    glfwSetWindowPosCallback(window, [](GLFWwindow* win, int x, int y)
    {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(mon, &mx, &my, &mw, &mh);

        int ww, wh;
        glfwGetWindowSize(win, &ww, &wh);

        int clampX = std::max(mx, std::min(x, mx + mw - ww));
        int clampY = std::max(my, std::min(y, my + mh - wh));

        if (clampX != x || clampY != y)
            glfwSetWindowPos(win, clampX, clampY);
    });

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "GLAD init failed\n"; return;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges      = true;

    // ── Style ─────────────────────────────────────────────────────
    ImGui::StyleColorsDark();
    ImGuiStyle& style        = ImGui::GetStyle();
    style.WindowRounding     = 6.0f;
    style.FrameRounding      = 4.0f;
    style.ScrollbarRounding  = 4.0f;
    style.GrabRounding       = 4.0f;
    style.WindowPadding      = ImVec2(10, 10);
    style.FramePadding       = ImVec2(6, 4);
    style.ItemSpacing        = ImVec2(8, 6);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        RenderUI(shared, networkManager);

        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

// ─────────────────────────────────────────────────────────────────
void RenderUI(SharedData& shared, NetworkManager& networkManager)
{
    // ── Snapshot ──────────────────────────────────────────────────
    static std::vector<ClientInfo>  localClients;
    static std::vector<std::string> localLog;
    static std::vector<DataPoint>   localPlot;

    if (shared.newDataReady)
    {
        std::lock_guard<std::mutex> lock(shared.mtx);
        localClients = shared.clientList;
        localLog     = shared.messageLog;
        localPlot    = shared.plotBuffer;
        shared.newDataReady = false;
    }

    bool running = shared.running.load();

    //------------------------------------------------
    // DOCKSPACE HOST - covers full viewport
    //------------------------------------------------
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos     (viewport->Pos);
    ImGui::SetNextWindowSize    (viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar             |
        ImGuiWindowFlags_NoCollapse             |
        ImGuiWindowFlags_NoResize               |
        ImGuiWindowFlags_NoMove                 |
        ImGuiWindowFlags_NoBringToFrontOnFocus  |
        ImGuiWindowFlags_NoNavFocus             |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

    // ── Menu bar ──────────────────────────────────────────────────
    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Exit"))
                glfwSetWindowShouldClose(glfwGetCurrentContext(), true);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Network"))
        {
            ImGui::MenuItem(
                running ? "* Running" : "* Stopped",
                nullptr, false, false
            );
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Reset Layout"))
                ImGui::LoadIniSettingsFromMemory("");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    // ── DockSpace ─────────────────────────────────────────────────
    ImGuiID dockID = ImGui::GetID("ServerDockSpace");

    ImGui::DockSpace(
        dockID,
        ImVec2(0, 0),
        ImGuiDockNodeFlags_PassthruCentralNode);

    static bool firstLayout = true;

    if (firstLayout)
    {
        firstLayout = false;

        ImGui::DockBuilderRemoveNode(dockID);
        ImGui::DockBuilderAddNode(
            dockID,
            ImGuiDockNodeFlags_DockSpace);

        ImGui::DockBuilderSetNodeSize(
            dockID,
            viewport->Size);

        ImGuiID dockMain  = dockID;
        ImGuiID dockLeft  = 0;
        ImGuiID dockDown  = 0;

        dockLeft = ImGui::DockBuilderSplitNode(
            dockMain,
            ImGuiDir_Left,
            0.25f,
            nullptr,
            &dockMain);

        dockDown = ImGui::DockBuilderSplitNode(
            dockMain,
            ImGuiDir_Down,
            0.35f,
            nullptr,
            &dockMain);

        ImGui::DockBuilderDockWindow(
            "Control Panel",
            dockLeft);

        ImGui::DockBuilderDockWindow(
            "Visualization",
            dockMain);

        ImGui::DockBuilderDockWindow(
            "Console",
            dockDown);

        ImGui::DockBuilderFinish(dockID);
    }

    ImGui::End();

    //------------------------------------------------
    // CONTROL PANEL
    //------------------------------------------------
    ImGui::Begin("Control Panel");

        ImGui::Text("Server Controls");
        ImGui::Separator();

        // ── Status indicator ──────────────────────────────────────
        ImGui::TextColored(
            running ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1),
            running ? "* Running" : "* Stopped"
        );

        if (running)
        {
            // Show current bound address
            ImGui::TextDisabled("  %s : %d",
                networkManager.getIP().c_str(),
                networkManager.getPort());
        }

        ImGui::Spacing();
        ImGui::Text("Clients:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1,1,0,1), "%d",
                           shared.clientCount.load());
        ImGui::Text("Packets/sec:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0,1,1,1), "%d",
                           shared.packetPerSecond.load());

        ImGui::Separator();
        ImGui::Spacing();

        // ── IP + Port - only editable when stopped ────────────────
        static char bindIP[64] = "0.0.0.0";
        static int  bindPort   = 9000;
        static char errorMsg[128] = {};

        ImGui::Text("Bind Address");
        ImGui::Spacing();

        ImGui::BeginDisabled(running);

            // IP selector
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##bindip", bindIP, sizeof(bindIP));
            ImGui::TextDisabled("  0.0.0.0 = all interfaces");

            ImGui::Spacing();

            // Port
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt("##bindport", &bindPort);
            if (bindPort < 1)    bindPort = 1;
            if (bindPort > 65535) bindPort = 65535;
            ImGui::TextDisabled("  Port: 1 - 65535");

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Start / Stop buttons ──────────────────────────────────
        ImGui::BeginDisabled(running);
            if (ImGui::Button("Start Server", ImVec2(-1, 0)))
            {
                errorMsg[0] = '\0';
                try
                {
                    networkManager.start(
                        std::string(bindIP), bindPort);
                }
                catch (const std::exception& e)
                {
                    snprintf(errorMsg, sizeof(errorMsg),
                             "%s", e.what());
                }
            }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!running);
            if (ImGui::Button("Stop Server", ImVec2(-1, 0)))
            {
                networkManager.stop();
            }
        ImGui::EndDisabled();

        // Error message
        if (errorMsg[0] != '\0')
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),
                               "%s", errorMsg);
        }

        ImGui::Separator();
        ImGui::Spacing();

        // ── Quick IP presets ──────────────────────────────────────
        ImGui::BeginDisabled(running);

            ImGui::Text("Quick Presets:");
            ImGui::Spacing();

            if (ImGui::Button("All Interfaces", ImVec2(-1, 0)))
                strncpy(bindIP, "0.0.0.0", sizeof(bindIP));

            if (ImGui::Button("Localhost only", ImVec2(-1, 0)))
                strncpy(bindIP, "127.0.0.1", sizeof(bindIP));

            // Detect and show local LAN IP
            static char lanIP[64] = {};
            if (lanIP[0] == '\0')
            {
                // Get local IP via hostname
                char hostname[256];
                if (gethostname(hostname, sizeof(hostname)) == 0)
                {
                    addrinfo hints = {}, *res = nullptr;
                    hints.ai_family   = AF_INET;
                    hints.ai_socktype = SOCK_STREAM;
                    if (getaddrinfo(hostname, nullptr, &hints, &res) == 0)
                    {
                        sockaddr_in* sa =
                            reinterpret_cast<sockaddr_in*>(res->ai_addr);
                        inet_ntop(AF_INET, &sa->sin_addr,
                                  lanIP, sizeof(lanIP));
                        freeaddrinfo(res);
                    }
                }
                if (lanIP[0] == '\0')
                    strncpy(lanIP, "unavailable", sizeof(lanIP));
            }

            // Show LAN IP as a clickable button
            std::string lanLabel = "LAN: ";
            lanLabel += lanIP;
            ImGui::BeginDisabled(std::string(lanIP) == "unavailable");
                if (ImGui::Button(lanLabel.c_str(), ImVec2(-1, 0)))
                    strncpy(bindIP, lanIP, sizeof(bindIP));
            ImGui::EndDisabled();

        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Spacing();

        // ── Active clients table ──────────────────────────────────
        ImGui::Text("Active Clients");
        ImGui::Spacing();

        if (localClients.empty())
        {
            ImGui::TextDisabled("No clients connected");
        }
        else if (ImGui::BeginTable("clients", 5,   // ← 5 columns now
            ImGuiTableFlags_Borders         |
            ImGuiTableFlags_RowBg           |
            ImGuiTableFlags_ScrollY         |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, 200)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("ID",   ImGuiTableColumnFlags_WidthFixed,   28);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("IP",   ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed,   48);
            ImGui::TableSetupColumn("Msgs", ImGuiTableColumnFlags_WidthFixed,   40);
            ImGui::TableHeadersRow();

            for (const auto& c : localClients)
            {
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.4f,0.8f,1,1), "#%d", c.id);

                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(1,0.85f,0.4f,1),
                    "%s", c.username.c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", c.ip.c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%d", c.port);

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%d", c.msgCount);
            }
            ImGui::EndTable();
        }

    ImGui::End();

    //------------------------------------------------
    // VISUALIZATION
    //------------------------------------------------
    ImGui::Begin("Visualization");

        ImGui::Text("Server Statistics");
        ImGui::Separator();

        static float clientHistory[100] = {};
        static int   chOffset           = 0;
        clientHistory[chOffset] =
            static_cast<float>(shared.clientCount.load());
        chOffset = (chOffset + 1) % 100;

        ImGui::Text("Clients Connected");
        ImGui::PlotLines("##clients",
            clientHistory, 100, chOffset, nullptr,
            0.0f, 20.0f,
            ImVec2(ImGui::GetContentRegionAvail().x, 70));

        static float packetHistory[100] = {};
        static int   phOffset           = 0;
        packetHistory[phOffset] =
            static_cast<float>(shared.packetPerSecond.load());
        phOffset = (phOffset + 1) % 100;

        ImGui::Text("Packets / sec");
        ImGui::PlotHistogram("##packets",
            packetHistory, 100, phOffset, nullptr,
            0.0f, 1000.0f,
            ImVec2(ImGui::GetContentRegionAvail().x, 70));

        if (!localPlot.empty())
        {
            ImGui::Separator();
            ImGui::Text("Per-Client Traffic");
            ImGui::Spacing();

            std::vector<int> seenIDs;
            for (const auto& p : localPlot)
                if (std::find(seenIDs.begin(), seenIDs.end(), p.clientID)
                    == seenIDs.end())
                    seenIDs.push_back(p.clientID);

            for (int id : seenIDs)
            {
                std::vector<float> vals;
                for (const auto& p : localPlot)
                    if (p.clientID == id)
                        vals.push_back(static_cast<float>(p.value));

                if (vals.empty()) continue;

                float maxVal = *std::max_element(
                    vals.begin(), vals.end());
                std::string label = "Client #" + std::to_string(id);

                ImGui::Text("%s", label.c_str());
                ImGui::PlotLines(
                    ("##p" + std::to_string(id)).c_str(),
                    vals.data(),
                    static_cast<int>(vals.size()),
                    0, nullptr, 0.0f, maxVal + 1.0f,
                    ImVec2(ImGui::GetContentRegionAvail().x, 80));
            }
        }

    ImGui::End();

    //------------------------------------------------
    // CONSOLE
    //------------------------------------------------
    ImGui::Begin("Console");

        float inputAreaHeight = 36.0f;
        ImGui::BeginChild("##log",
            ImVec2(0, ImGui::GetContentRegionAvail().y - inputAreaHeight),
            false, ImGuiWindowFlags_HorizontalScrollbar);

            int start = (int)localLog.size() > 100
                        ? (int)localLog.size() - 100 : 0;

            for (int i = start; i < (int)localLog.size(); i++)
            {
                const std::string& msg = localLog[i];
                if      (msg.find("[+]")         != std::string::npos)
                    ImGui::TextColored(ImVec4(0.2f,1,0.2f,1),   "%s", msg.c_str());
                else if (msg.find("[-]")         != std::string::npos)
                    ImGui::TextColored(ImVec4(1,0.4f,0.4f,1),   "%s", msg.c_str());
                else if (msg.find("[BROADCAST]") != std::string::npos)
                    ImGui::TextColored(ImVec4(1,1,0.2f,1),      "%s", msg.c_str());
                else if (msg.find("[RELAY")      != std::string::npos)
                    ImGui::TextColored(ImVec4(1,0.6f,0,1),      "%s", msg.c_str());
                else if (msg.find("[IMG]")       != std::string::npos)
                    ImGui::TextColored(ImVec4(0.8f,0.5f,1,1),   "%s", msg.c_str());
                else
                    ImGui::TextUnformatted(msg.c_str());
            }

            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Send Broadcast Message:");

        static char sendBuf[512] = {};

        ImGui::SetNextItemWidth(
            ImGui::GetContentRegionAvail().x - 80);
        bool hitEnter = ImGui::InputText(
            "##input", sendBuf, sizeof(sendBuf),
            ImGuiInputTextFlags_EnterReturnsTrue
        );
        ImGui::SameLine();
        bool hitSend = ImGui::Button("Send", ImVec2(70, 0));

        if ((hitEnter || hitSend) && sendBuf[0] != '\0')
        {
            networkManager.broadCasting(std::string(sendBuf));
            sendBuf[0] = '\0';
            ImGui::SetKeyboardFocusHere(-1);
        }

    ImGui::End();
}