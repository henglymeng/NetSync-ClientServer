#include "glad/glad.h"
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui_internal.h"

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include <map>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <commdlg.h>

#include "sharedData/sharedDataClient.h"
#include "Thread/networkManagerClient.h"

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT  9000

void framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    glViewport(0, 0, width, height);
}

void GUIClient(SharedDataClient& shared, NetworkManagerClient& network);
void RenderUIClient(SharedDataClient& shared, NetworkManagerClient& network);

// ─────────────────────────────────────────────────────────────────
int main()
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    SharedDataClient     shared;
    NetworkManagerClient network(shared);

    GUIClient(shared, network);

    network.disconnect();
    WSACleanup();
    return 0;
}

void GUIClient(SharedDataClient& shared, NetworkManagerClient& network)
{
    if (!glfwInit()) { std::cerr << "GLFW init failed\n"; return; }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWmonitor*       monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode    = glfwGetVideoMode(monitor);

    int monX, monY, monW, monH;
    glfwGetMonitorWorkarea(monitor, &monX, &monY, &monW, &monH);

    int winW = static_cast<int>(monW * 0.8f);
    int winH = static_cast<int>(monH * 0.8f);
    int winX = monX + (monW - winW) / 2;
    int winY = monY + (monH - winH) / 2;

    GLFWwindow* window = glfwCreateWindow(
        winW, winH, "NetSync - Client", nullptr, nullptr);
    if (!window) { glfwTerminate(); return; }

    glfwSetWindowPos(window, winX, winY);
    glfwSetWindowSizeLimits(window,
        640, 480, GLFW_DONT_CARE, GLFW_DONT_CARE);

    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSwapInterval(1);

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

    gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigWindowsResizeFromEdges      = true;

    ImGui::StyleColorsDark();
    ImGuiStyle& style        = ImGui::GetStyle();
    style.WindowRounding     = 6.0f;
    style.FrameRounding      = 4.0f;
    style.ScrollbarRounding  = 4.0f;
    style.GrabRounding       = 4.0f;
    style.WindowPadding      = ImVec2(10, 10);
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

        RenderUIClient(shared, network);

        ImGui::Render();
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
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
// ─────────────────────────────────────────────────────────────────
void RenderUIClient(SharedDataClient& shared, NetworkManagerClient& network)
{
    // ── ONE snapshot block - all locals updated together ──────────
    static std::map<std::string, std::vector<ChatMessage>> localChat;
    static std::vector<ReceivedMessage>       localLog;
    static std::vector<float>                 localPlot;

    if (shared.newDataReady)  // ← checked once, at the top
    {
        std::lock_guard<std::mutex> lock(shared.mtx);
        localChat = shared.chatHistory;   
        localLog  = shared.messageLog;
        localPlot = shared.plotBuffer;
        shared.newDataReady = false;      
    }

    bool connected = shared.connected.load();

    // ── PERSISTENT BACKEND CONTROLS FOR MODERN UX ────────────────
    // Note: Bind these to your network packet parser in production
    static std::map<std::string, int> unreadCount; 
    static std::map<std::string, bool> isTyping;   

    //------------------------------------------------
    // DOCKSPACE HOST
    //------------------------------------------------
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos     (viewport->Pos);
    ImGui::SetNextWindowSize    (viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoTitleBar            |
        ImGuiWindowFlags_NoCollapse            |
        ImGuiWindowFlags_NoResize              |
        ImGuiWindowFlags_NoMove                |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus            |
        ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);

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
            ImGui::MenuItem(connected ? "* Connected" : "* Disconnected", nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View"))
        {
            if (ImGui::MenuItem("Reset Layout"))
                ImGui::LoadIniSettingsFromMemory("");
            ImGui::EndMenu();
        }

        // Username right side of menu bar
        std::string nameTag = "  " + network.getUsername() + "  ";
        float avail = ImGui::GetContentRegionAvail().x;
        float nameW = ImGui::CalcTextSize(nameTag.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - nameW);
        ImGui::TextColored(ImVec4(1,1,0.4f,1), "%s", nameTag.c_str());

        ImGui::EndMenuBar();
    }

    ImGuiID dockID = ImGui::GetID("ClientDockSpace");
    ImGui::DockSpace(dockID, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);

    static bool firstLayout = true;
    if (firstLayout)
    {
        firstLayout = false;
        ImGui::DockBuilderRemoveNode(dockID);
        ImGui::DockBuilderAddNode(dockID, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockID, viewport->Size);

        ImGuiID dockMain  = dockID;
        ImGuiID dockLeft  = 0;
        ImGuiID dockRight = 0;
        ImGuiID dockDown  = 0;

        dockLeft = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Left, 0.22f, nullptr, &dockMain);
        dockRight = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Right, 0.22f, nullptr, &dockMain);
        dockDown = ImGui::DockBuilderSplitNode(dockMain, ImGuiDir_Down, 0.28f, nullptr, &dockMain);

        ImGui::DockBuilderDockWindow("Control Panel", dockLeft);
        ImGui::DockBuilderDockWindow("System Log",    dockRight);
        ImGui::DockBuilderDockWindow("Chat",          dockMain);
        ImGui::DockBuilderDockWindow("Visualization", dockDown);
        ImGui::DockBuilderFinish(dockID);
    }
    ImGui::End();

    //------------------------------------------------
    // CONTROL PANEL
    //------------------------------------------------
    ImGui::Begin("Control Panel");
        ImGui::TextColored(connected ? ImVec4(0,1,0,1) : ImVec4(1,0.3f,0.3f,1), connected ? "* Connected" : "* Disconnected");
        ImGui::Separator();
        ImGui::Spacing();

        static char ip[64] = DEFAULT_IP;
        static int  port    = DEFAULT_PORT;

        ImGui::BeginDisabled(connected);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##ip",   ip,   sizeof(ip));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputInt ("##port", &port);
            if (port < 1)     port = 1;
            if (port > 65535) port = 65535;
        ImGui::EndDisabled();

        ImGui::Spacing();

        ImGui::BeginDisabled(connected);
            if (ImGui::Button("Connect", ImVec2(-1, 0)))
            {
                try { network.connect(std::string(ip), port); }
                catch (const std::exception& e)
                {
                    std::lock_guard<std::mutex> lock(shared.mtx);
                    shared.messageLog.push_back({"[ERR] " + std::string(e.what()), 0.0});
                    shared.newDataReady = true;
                }
            }
        ImGui::EndDisabled();

        ImGui::BeginDisabled(!connected);
            if (ImGui::Button("Disconnect", ImVec2(-1, 0)))
                network.disconnect();
        ImGui::EndDisabled();

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Sent:  %d bytes", shared.byteSent.load());
        ImGui::Text("Recv:  %d bytes", shared.byteReceived.load());

        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Identity");
        ImGui::Spacing();

        static char usernameBuf[64] = "Client";
        static bool nameInit        = false;
        if (!nameInit)
        {
            strncpy(usernameBuf, network.getUsername().c_str(), sizeof(usernameBuf) - 1);
            nameInit = true;
        }

        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##uname", usernameBuf, sizeof(usernameBuf));

        if (ImGui::Button("Set Name", ImVec2(-1, 0)) && usernameBuf[0] != '\0')
            network.setUsername(std::string(usernameBuf));

        ImGui::TextDisabled("Current: %s", network.getUsername().c_str());

        ImGui::Separator();
        ImGui::Spacing();
    ImGui::End();

    //---------------------------------------------------------------------
    // CHAT WINDOW (Unified Modern UX Version)
    //---------------------------------------------------------------------
    ImGui::Begin("Chat");

        static std::string activeChat = "ALL";
        float contactW = 140.0f; 

        // ── LEFT: Contact list ──────────────────────────────────────────
        ImGui::BeginChild("##contacts", ImVec2(contactW, ImGui::GetContentRegionAvail().y), true);

            int totalThreads = (int)localChat.size() - (localChat.count("__LIST__") ? 1 : 0);
            ImGui::Text("Contacts (%d)", std::max(0, totalThreads));
            ImGui::Separator();
            ImGui::Spacing();

            // ALL / Broadcast Item Layout
            {
                bool sel = (activeChat == "ALL");
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.40f, 0.75f, 1.0f));

                std::string allLabel = "ALL";
                if (unreadCount["ALL"] > 0 && !sel) {
                    allLabel += " (" + std::to_string(unreadCount["ALL"]) + ")";
                }

                if (ImGui::Button(allLabel.c_str(), ImVec2(-1, 0))) {
                    activeChat = "ALL";
                    unreadCount["ALL"] = 0; 
                }
                if (sel) ImGui::PopStyleColor();
                
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Broadcast to all users");
            }

            ImGui::Spacing();

            // Per-client Active Tabs
            for (const auto& [key, msgs] : localChat)
            {
                if (key == "ALL" || key == "__LIST__") continue;

                bool sel = (activeChat == key);
                if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.40f, 0.75f, 1.0f));

                std::string clientLabel = key;
                if (unreadCount[key] > 0 && !sel) {
                    clientLabel += " (" + std::to_string(unreadCount[key]) + ")";
                }

                if (ImGui::Button(clientLabel.c_str(), ImVec2(-1, 0))) {
                    activeChat = key;
                    unreadCount[key] = 0; 
                }
                if (sel) ImGui::PopStyleColor();

                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Direct chat with %s", key.c_str());
            }
            
        ImGui::EndChild();
        
        ImGui::SameLine();

        // ── RIGHT: Message area ─────────────────────────────────────────
        ImGui::BeginGroup();

            std::string headerTitle = "";
            std::string subHeader = "";

            if (activeChat == "ALL") {
                headerTitle = "Broadcast - ALL";
                subHeader = "Broadcast to all connected users";
            } else if (activeChat == "__LIST__") {
                headerTitle = "Connected Users";
                subHeader = "Global network directory listing";
            } else {
                headerTitle = "Direct - " + activeChat;
                subHeader = "Private conversation with " + activeChat;
            }

            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "%s", headerTitle.c_str());
            ImGui::SameLine();
            if (localChat.count(activeChat) && activeChat != "__LIST__") {
                ImGui::TextDisabled("  %zu messages", localChat[activeChat].size());
            }
            
            ImGui::TextDisabled("%s", subHeader.c_str());
            ImGui::Separator();

            // Sizing allocations matching the container constraints safely
            float sendBarH = 26.0f;
            float typingIndicatorH = ImGui::GetTextLineHeightWithSpacing();
            float reservedSpace = sendBarH + typingIndicatorH + 10.0f;

            ImGui::BeginChild("##msgs", ImVec2(0, ImGui::GetContentRegionAvail().y - reservedSpace), false);
            
            if (activeChat == "__LIST__")
            {
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "Online Users Directory");
                ImGui::Separator();
                ImGui::Spacing();

                for (const auto& [name, msgs] : localChat)
                {
                    if (name == "ALL" || name == "__LIST__") continue;

                    if (ImGui::Selectable(name.c_str())) {
                        activeChat = name;
                        unreadCount[name] = 0;
                    }
                    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        activeChat = name;
                        unreadCount[name] = 0;
                    }
                }
            }
            else
            {
                auto& msgs = localChat[activeChat];
                int startIdx = (int)msgs.size() > 200 ? (int)msgs.size() - 200 : 0;

                // Set up sleek spacing and rounding configurations for message bubbles
                float paddingX = 12.0f;
                float paddingY = 8.0f;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(paddingX, paddingY));
                ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);

                for (int i = startIdx; i < (int)msgs.size(); i++)
                {
                    const ChatMessage& cm = msgs[i];
                    float availWidth = ImGui::GetContentRegionAvail().x;

                    // Parse and handle time configurations safely
                    char timeStr[16] = "";
                    if (cm.timestamp > 0.0) {
                        std::time_t rawTime = static_cast<std::time_t>(cm.timestamp);
                        std::tm* timeInfo = std::localtime(&rawTime);
                        if (timeInfo) {
                            std::strftime(timeStr, sizeof(timeStr), "%H:%M", timeInfo);
                        }
                    }

                    if (cm.isOwn)
                    {
                        // ── OUTBOUND MESSAGE BUBBLE (RIGHT ALIGNED) ──
                        float maxBubbleWidth = availWidth * 0.70f;
                        float textWidth = ImGui::CalcTextSize(cm.body.c_str()).x;
                        float boxW = std::min(maxBubbleWidth, textWidth + (paddingX * 2.0f));
                        
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, availWidth - boxW - 4.0f));

                        // Modern, rich signature blue accent color for sent bubbles
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.35f, 0.62f, 0.90f));
                        ImGui::BeginChild(("b_own_" + std::to_string(i)).c_str(), ImVec2(boxW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
                            
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boxW - (paddingX * 2.0f));
                                ImGui::TextUnformatted(cm.body.c_str());
                            ImGui::PopTextWrapPos();
                            
                            if (timeStr[0] != '\0') {
                                float tsWidth = ImGui::CalcTextSize(timeStr).x;
                                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - tsWidth - paddingX);
                                ImGui::TextDisabled("%s", timeStr);
                            }

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    else if (cm.sender == "System" || cm.sender == "Server")
                    {
                        // ── CENTERED NOTIFICATION SLOTS ──
                        // Strips out flat text prefix repetitions and balances it in the window center
                        float textWidth = ImGui::CalcTextSize(cm.body.c_str()).x;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (availWidth - textWidth) / 2.0f));
                        
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.30f, 0.85f, 0.95f, 0.85f));
                        ImGui::TextUnformatted(cm.body.c_str());
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        // ── INBOUND MESSAGE BUBBLE (LEFT ALIGNED) ──
                        float maxBubbleWidth = availWidth * 0.70f;
                        float textWidth = ImGui::CalcTextSize(cm.body.c_str()).x;
                        float boxW = std::min(maxBubbleWidth, textWidth + (paddingX * 2.0f));

                        // Smooth charcoal/slate background palette for incoming bubbles
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.20f, 0.23f, 1.0f));
                        ImGui::BeginChild(("b_in_" + std::to_string(i)).c_str(), ImVec2(boxW, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoScrollbar);
                            
                            // Clean Username Header
                            ImGui::TextColored(ImVec4(0.40f, 0.85f, 1.0f, 1.0f), "%s", cm.sender.c_str());
                            ImGui::Separator();
                            
                            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + boxW - (paddingX * 2.0f));
                                ImGui::TextUnformatted(cm.body.c_str());
                            ImGui::PopTextWrapPos();
                            
                            if (timeStr[0] != '\0') {
                                float tsWidth = ImGui::CalcTextSize(timeStr).x;
                                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - tsWidth - paddingX);
                                ImGui::TextDisabled("%s", timeStr);
                            }

                        ImGui::EndChild();
                        ImGui::PopStyleColor();
                    }
                    ImGui::Spacing();
                }

                ImGui::PopStyleVar(2);

                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) 
                    ImGui::SetScrollHereY(1.0f);
            }

            ImGui::EndChild();

            ImGui::Separator();

            // Typing Indicator Spacer
            if (activeChat != "__LIST__" && isTyping[activeChat]) {
                ImGui::TextDisabled("%s is typing...", activeChat.c_str());
            } else {
                ImGui::Dummy(ImVec2(0, typingIndicatorH));
            }

            // Image transfer progress indicator
            int imgProgress = shared.imgSendProgress.load();
            if (imgProgress > 0 && imgProgress < 100) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Sending image... %d%%", imgProgress);
                ImGui::ProgressBar(imgProgress / 100.0f, ImVec2(-1, 0));
            }

            // ── DYNAMIC BAR INPUT SYSTEM WITH INTEGRATED FILE ATTACHMENTS ──
            static char chatBuf[512] = {};
            bool canSend = connected && (activeChat != "__LIST__");

            ImGui::BeginDisabled(!canSend);

                // Who button to show active users in the current thread (for future group chat support)
                if (ImGui::Button("WHO", ImVec2(40, 0)))
                {
                    network.sendWho();
                }

                ImGui::SameLine();

                // File Attachment Button Component
                if (ImGui::Button("image", ImVec2(40, 0)))
                {
                    OPENFILENAMEA ofn = {};
                    char fileDest[512] = {};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.lpstrFilter = "All Files\0*.*\0Documents\0*.pdf;*.docx;*.txt\0";
                    ofn.lpstrFile = fileDest;
                    ofn.nMaxFile = sizeof(fileDest);
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                    
                    if (GetOpenFileNameA(&ofn))
                    {
                        network.sendImage(std::string(fileDest));
                    }
                }
                
                ImGui::SameLine();

                // Auto-calculate exact space remaining for input and send actions
                float remainingSpace = ImGui::GetContentRegionAvail().x;
                ImGui::SetNextItemWidth(remainingSpace - 70.0f);

                bool hitEnter = ImGui::InputTextWithHint("##chatinput", "Type a message...", chatBuf, sizeof(chatBuf), ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                bool hitSend = ImGui::Button("Send", ImVec2(60, 0));

                if ((hitEnter || hitSend) && chatBuf[0] != '\0')
                {
                    // Convert username to client ID format
                    std::string target = activeChat;
                    if (activeChat != "ALL")
                    {
                        auto it = shared.usernameToID.find(activeChat);
                        if (it != shared.usernameToID.end())
                        {
                            target = it->second;
                        }
                    }
                    network.sendMessage("TO:" + target + " " + std::string(chatBuf));
                    chatBuf[0] = '\0';
                    ImGui::SetKeyboardFocusHere(-1);
                }

            ImGui::EndDisabled();
        ImGui::EndGroup();
    ImGui::End();
    
    //------------------------------------------------
    // VISUALIZATION
    //------------------------------------------------
    ImGui::Begin("Visualization");
        if (localPlot.size() > 1)
        {
            float maxVal = *std::max_element(localPlot.begin(), localPlot.end());

            ImGui::Text("Signal");
            ImGui::PlotLines("##recv", localPlot.data(), static_cast<int>(localPlot.size()), 0, nullptr, 0.0f, maxVal + 1.0f, ImVec2(ImGui::GetContentRegionAvail().x, 60));

            static float recvHistory[100] = {};
            static int   rhOffset         = 0;
            recvHistory[rhOffset] = static_cast<float>(shared.byteReceived.load());
            rhOffset = (rhOffset + 1) % 100;

            ImGui::Text("Bytes received");
            ImGui::PlotHistogram("##brecv", recvHistory, 100, rhOffset, nullptr, 0.0f, 10000.0f, ImVec2(ImGui::GetContentRegionAvail().x, 60));
        }
        else
        {
            ImGui::TextDisabled("Connect and send a message.");
        }
    ImGui::End();

    //------------------------------------------------
    // SYSTEM LOG
    //------------------------------------------------
    ImGui::Begin("System Log");
        ImGui::BeginChild("##syslog", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        
            int s2 = (int)localLog.size() > 100 ? (int)localLog.size() - 100 : 0;
            for (int i = s2; i < (int)localLog.size(); i++)
            {
                const std::string& m = localLog[i].text;
                if      (m.find("[+]")  != std::string::npos) ImGui::TextColored(ImVec4(0.2f,1,0.2f,1), "%s", m.c_str());
                else if (m.find("[-]")  != std::string::npos) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "%s", m.c_str());
                else if (m.find("[ERR]") != std::string::npos) ImGui::TextColored(ImVec4(1,0.2f,0.2f,1), "%s", m.c_str());
                else if (m.find("[IMG]") != std::string::npos) ImGui::TextColored(ImVec4(0.8f,0.5f,1,1), "%s", m.c_str());
                else if (m.find("[~]")  != std::string::npos) ImGui::TextColored(ImVec4(1,0.85f,0.4f,1), "%s", m.c_str());
                else                                          ImGui::TextUnformatted(m.c_str());
            }

            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    ImGui::End();
}