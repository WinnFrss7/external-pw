#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <iostream>

DWORD GetProcId(const char* procName);
uintptr_t FindPattern(HANDLE hProcess, const std::vector<BYTE>& pattern, const std::string& mask);

int main()
{
    const int WINDOW_WIDTH = 900;
    const int WINDOW_HEIGHT = 620;

    glfwInit();
    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Pixel Worlds Cheat Menu", NULL, NULL);
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.1f, 0.4f, 0.8f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    DWORD procID = 0;
    HANDLE hProcess = NULL;
    uintptr_t funcAddress = 0;

    std::vector<BYTE> originalBytes = {
        0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x0F, 0x29, 0x74, 0x24, 0x40,
        0x48, 0x8B, 0xD9, 0x0F, 0x57, 0xF6, 0x0F, 0x2F, 0xB1, 0x44, 0x01,
        0x00, 0x00, 0x77, 0x27
    };

    bool freezeFish = false;

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));

        ImGui::Begin("Pixel Worlds Cheat Menu", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        ImGui::TextColored(ImVec4(0.0f, 0.8f, 1.0f, 1.0f), "Pixel Worlds External Cheat");
        ImGui::Separator();

        if (ImGui::Button("Attach + Scan Function", ImVec2(300, 40)))
        {
            procID = GetProcId("PixelWorlds.exe");
            if (procID)
            {
                hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procID);
                if (hProcess)
                {
                    std::string mask = "xxxxxxxxxxxxxxxxxxxxxxxxxx";
                    funcAddress = FindPattern(hProcess, originalBytes, mask);

                    if (funcAddress)
                        std::cout << "[+] Success! Address: 0x" << std::hex << funcAddress << std::endl;
                    else
                        std::cout << "[-] Pattern not found!\n";
                }
            }
        }

        ImGui::SameLine();
        ImGui::Text("Status: %s", hProcess ? "ATTACHED" : "Not Attached");

        if (funcAddress)
            ImGui::TextColored(ImVec4(0, 1, 0, 1), "Function Address: 0x%llX", funcAddress);

        ImGui::Separator();

        ImGui::BeginGroup();
        ImGui::Text("Fishing Cheats");
        ImGui::Separator();

        if (ImGui::Checkbox("Freeze Fish Position", &freezeFish) && hProcess && funcAddress)
        {
            if (freezeFish)
            {
                BYTE patch[] = { 0xC3, 0x90 }; // RET, NOP
                WriteProcessMemory(hProcess, (LPVOID)funcAddress, patch, 2, NULL);
            }
            else
            {
                WriteProcessMemory(hProcess, (LPVOID)funcAddress, originalBytes.data(), originalBytes.size(), NULL);
            }
        }

        ImGui::EndGroup();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.8f, 0, 1), "Tip: Click Attach first, then enable cheats.");

        ImGui::End();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    if (hProcess) CloseHandle(hProcess);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

// Helpers
DWORD GetProcId(const char* procName)
{
    PROCESSENTRY32 pe32{ sizeof(PROCESSENTRY32) };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Process32First(hSnap, &pe32)) {
        do {
            if (_stricmp(pe32.szExeFile, procName) == 0) {
                CloseHandle(hSnap);
                return pe32.th32ProcessID;
            }
        } while (Process32Next(hSnap, &pe32));
    }
    CloseHandle(hSnap);
    return 0;
}

uintptr_t FindPattern(HANDLE hProcess, const std::vector<BYTE>& pattern, const std::string& mask)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;

    while (addr < (uintptr_t)si.lpMaximumApplicationAddress)
    {
        if (VirtualQueryEx(hProcess, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;

        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        {
            std::vector<BYTE> buffer(mbi.RegionSize);
            SIZE_T bytesRead;
            if (ReadProcessMemory(hProcess, (LPCVOID)addr, buffer.data(), mbi.RegionSize, &bytesRead))
            {
                for (size_t i = 0; i < bytesRead - pattern.size(); ++i)
                {
                    bool match = true;
                    for (size_t j = 0; j < pattern.size(); ++j)
                    {
                        if (mask[j] == 'x' && buffer[i + j] != pattern[j])
                        {
                            match = false;
                            break;
                        }
                    }
                    if (match) return addr + i;
                }
            }
        }
        addr += mbi.RegionSize;
    }
    return 0;
}