#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
#include <cstdint>

#define IDI_LOGO 101

// ─────────────────────────────────────────────
//  Process / module state
// ─────────────────────────────────────────────
DWORD   procID = 0;
HANDLE  hProcess = NULL;

// ─────────────────────────────────────────────
//  Feature addresses (0 = not found)
// ─────────────────────────────────────────────
uintptr_t fishingAddr = 0;
uintptr_t changeLightAddr = 0;
uintptr_t fogOfWarAddr = 0;

// ─────────────────────────────────────────────
//  Feature toggle flags
// ─────────────────────────────────────────────
bool freezeFish = false;
bool antiDarkness = false;   // ChangeLighting only — independent of fog
bool noFogOfWar = false;   // FogOfWar only       — independent of lighting

// ─────────────────────────────────────────────
//  Patterns + masks
// ─────────────────────────────────────────────

// Fishing — 26 bytes, all fixed
std::vector<BYTE> fishingPattern = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x50,
    0x0F, 0x29, 0x74, 0x24, 0x40,
    0x48, 0x8B, 0xD9, 0x0F, 0x57,
    0xF6, 0x0F, 0x2F, 0xB1, 0x44,
    0x01, 0x00, 0x00, 0x77, 0x27
};
const std::string fishingMask = "xxxxxxxxxxxxxxxxxxxxxxxxxx";

// WorldController.ChangeLighting — 24 bytes
// bytes 0-11  : fixed  (48 89 5C 24 08 57 48 83 EC 20 80 3D)
// bytes 12-15 : ptr wildcard (4-byte module-relative address)
// byte  16    : fixed  (00)
// bytes 17-23 : fixed  (8B FA 48 8B D9 75 29)
std::vector<BYTE> changeLightPattern = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x80, 0x3D,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x8B, 0xFA, 0x48, 0x8B, 0xD9, 0x75, 0x29,
    0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00, 0xE8, 0x00, 0x00, 0x00, 0x00,
    0xF0, 0x83, 0x0C, 0x24, 0x00, 0x48, 0x8D, 0x0D, 0x00, 0x00, 0x00, 0x00,
    0xE8, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x83, 0x0C, 0x24, 0x00, 0xC6, 0x05,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x48, 0x8B, 0x43, 0x38
};
const std::string changeLightMask = "xxxxxxxxxxxx?????xxxxxxxxxx????x????xxxxxxxx????x????xxxxxxx????xxxxx";

// Original restore bytes for each feature (only what we actually overwrite)
std::vector<BYTE> changeLightOriginal = { 0x48, 0x89, 0x5C, 0x24, 0x08 };

// FogOfWar — 22 bytes
// bytes 0-7   : fixed  (40 55 48 83 EC 70 80 3D)
// bytes 8-11  : ptr wildcard
// byte  12    : fixed  (00)
// bytes 13-21 : fixed  (48 8B E9 0F 85 A0 00 00 00)
std::vector<BYTE> fogOfWarPattern = {
    0x40, 0x55,                               // push rbp  (REX.B + push rbp)
    0x48, 0x83, 0xEC, 0x70,                   // sub rsp,70
    0x80, 0x3D,                               // cmp byte ptr [...]
    0x00, 0x00, 0x00, 0x00,                   // <ptr — wildcarded>
    0x00,                                      // ,00
    0x48, 0x8B, 0xE9,                         // mov rbp,rcx
    0x0F, 0x85, 0xA0, 0x00, 0x00, 0x00       // jne ...
};
const std::string fogOfWarMask = "xxxxxxxx????xxxxxxxxxx";

std::vector<BYTE> fogOfWarOriginal = { 0x40, 0x55 };

// ─────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────
DWORD     GetProcId(const char* procName);
uintptr_t GetModuleBase(DWORD pid, const char* moduleName, SIZE_T* outSize = nullptr);
uintptr_t FindPatternInModule(HANDLE hProc, DWORD pid, const char* moduleName,
    const std::vector<BYTE>& pattern, const std::string& mask);
uintptr_t FindPattern(HANDLE hProc,
    const std::vector<BYTE>& pattern, const std::string& mask);
GLuint    LoadTextureFromResource(int resourceId);

// ─────────────────────────────────────────────
//  Texture loader
// ─────────────────────────────────────────────
GLuint LoadTextureFromResource(int resourceId)
{
    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC   hRes = FindResource(hModule, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    if (!hRes) return 0;

    HGLOBAL hData = LoadResource(hModule, hRes);
    if (!hData) return 0;

    void* pData = LockResource(hData);
    DWORD size = SizeofResource(hModule, hRes);

    int width, height, channels;
    unsigned char* data = stbi_load_from_memory(
        (unsigned char*)pData, size, &width, &height, &channels, 4);
    if (!data) return 0;

    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, data);
    stbi_image_free(data);
    return texture;
}

// ─────────────────────────────────────────────
//  Helper: write a patch safely (restore first
//  byte only for 1-byte patches)
// ─────────────────────────────────────────────
static bool WritePatch(DWORD pid, uintptr_t addr, const BYTE* bytes, SIZE_T len)
{
    // Temporarily change page protection so we can write
    // (some executable regions are PAGE_EXECUTE_READ)
    DWORD oldProtect;
    VirtualProtectEx(hProcess, (LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &oldProtect);
    BOOL ok = WriteProcessMemory(hProcess, (LPVOID)addr, bytes, len, NULL);
    VirtualProtectEx(hProcess, (LPVOID)addr, len, oldProtect, &oldProtect);
    return ok != FALSE;
}

// ─────────────────────────────────────────────
//  Reset all state (called before each Attach)
// ─────────────────────────────────────────────
static void ResetState()
{
    if (hProcess) CloseHandle(hProcess);
    hProcess = NULL;
    procID = 0;
    fishingAddr = 0;
    changeLightAddr = 0;
    fogOfWarAddr = 0;
    freezeFish = false;
    antiDarkness = false;
    noFogOfWar = false;
}

// ─────────────────────────────────────────────
//  main
// ─────────────────────────────────────────────
int main()
{
    const int WINDOW_WIDTH = 980;
    const int WINDOW_HEIGHT = 640;

    glfwInit();

    GLFWwindow* window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "IMIHAX", NULL, NULL);
    if (!window)
    {
        std::cout << "Failed to create GLFW window" << std::endl;
        return -1;
    }

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
    style.WindowRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.GrabRounding = 8.0f;
    style.ChildRounding = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.03f, 0.03f, 0.03f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.06f, 0.06f, 0.06f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.75f, 0.05f, 0.05f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.10f, 0.10f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.20f, 0.20f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.50f, 0.05f, 0.05f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.08f, 0.08f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.90f, 0.10f, 0.10f, 1.0f);
    colors[ImGuiCol_TitleBg] = ImVec4(0.05f, 0.05f, 0.05f, 1.0f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.02f, 0.02f, 1.0f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    GLuint logoTexture = LoadTextureFromResource(IDI_LOGO);

    // ── Main loop ──────────────────────────────
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(WINDOW_WIDTH, WINDOW_HEIGHT));

        ImGui::Begin("IMIHAX", nullptr,
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar);

        ImGui::Columns(2, nullptr, false);
        ImGui::SetColumnWidth(0, 260);

        // ── LEFT PANEL ─────────────────────────
        ImGui::BeginChild("Sidebar", ImVec2(0, 0), true);

        if (logoTexture)
        {
            ImGui::SetCursorPosX(40);
            ImGui::Image((ImTextureID)(intptr_t)logoTexture, ImVec2(170, 170));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1, 0.1f, 0.1f, 1), "IMIHAX COMMUNITY");
        ImGui::Spacing();

        // Connection status
        ImGui::Text("Target:");
        ImGui::TextColored(
            hProcess ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0, 0, 1),
            hProcess ? "CONNECTED" : "DISCONNECTED");

        ImGui::Spacing();

        // Per-feature scan status
        auto StatusLabel = [](const char* label, uintptr_t addr) {
            ImGui::Text("%s:", label);
            ImGui::SameLine();
            ImGui::TextColored(
                addr ? ImVec4(0, 1, 0, 1) : ImVec4(1, 0.3f, 0.3f, 1),
                addr ? "FOUND" : "NOT FOUND");
            };

        StatusLabel("Fishing", fishingAddr);
        StatusLabel("ChangeLighting", changeLightAddr);
        StatusLabel("FogOfWar", fogOfWarAddr);

        ImGui::Spacing();

        // Attach + Scan — scans GameAssembly.dll first, falls back to full scan for fishing
        if (ImGui::Button("ATTACH + SCAN", ImVec2(220, 45)))
        {
            ResetState();

            procID = GetProcId("PixelWorlds.exe");

            if (procID)
            {
                hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, procID);

                if (hProcess)
                {
                    // Fishing: full memory scan (lives in GameAssembly too,
                    // but keeping original broad scan as it was working)
                    fishingAddr = FindPattern(hProcess, fishingPattern, fishingMask);
                    std::cout << (fishingAddr
                        ? "[+] Fishing        : 0x" + std::to_string(fishingAddr) + "\n"
                        : "[-] Fishing not found\n");

                    // ChangeLighting: scan only inside GameAssembly.dll
                    changeLightAddr = FindPatternInModule(
                        hProcess, procID, "GameAssembly.dll",
                        changeLightPattern, changeLightMask);
                    if (changeLightAddr)
                        std::cout << "[+] ChangeLighting : 0x" << std::hex << changeLightAddr << std::endl;
                    else
                        std::cout << "[-] ChangeLighting not found" << std::endl;

                    // FogOfWar: scan only inside GameAssembly.dll
                    fogOfWarAddr = FindPatternInModule(
                        hProcess, procID, "GameAssembly.dll",
                        fogOfWarPattern, fogOfWarMask);
                    if (fogOfWarAddr)
                        std::cout << "[+] FogOfWar       : 0x" << std::hex << fogOfWarAddr << std::endl;
                    else
                        std::cout << "[-] FogOfWar not found" << std::endl;
                }
            }
            else
            {
                std::cout << "[-] Process not found" << std::endl;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1, 0.2f, 0.2f, 1), "INFO");
        ImGui::Text("Version : v1.1");
        ImGui::Text("Engine  : ImGui");
        ImGui::Text("Type    : External");

        ImGui::EndChild();

        ImGui::NextColumn();

        // ── MAIN PANEL ─────────────────────────
        ImGui::BeginChild("MainPanel", ImVec2(0, 0), true);

        ImGui::TextColored(ImVec4(1, 0.1f, 0.1f, 1), "PIXEL WORLDS MODULE");
        ImGui::Separator();
        ImGui::Spacing();

        // ── Fishing ────────────────────────────
        ImGui::Text("Fishing Cheats");
        ImGui::Spacing();

        {
            bool fishReady = hProcess && fishingAddr;
            if (!fishReady) ImGui::BeginDisabled();

            if (ImGui::Checkbox("Freeze Fish Position", &freezeFish) && fishReady)
            {
                if (freezeFish)
                {
                    BYTE patch[] = { 0xC3, 0x90 };
                    WritePatch(procID, fishingAddr, patch, sizeof(patch));
                }
                else
                {
                    WritePatch(procID, fishingAddr,
                        fishingPattern.data(), fishingPattern.size());
                }
            }

            if (!fishReady) ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── World / Lighting ───────────────────
        ImGui::Text("World Cheats");
        ImGui::Spacing();

        // Anti Darkness — patches ChangeLighting only, no FogOfWar dependency
        {
            bool lightReady = hProcess && changeLightAddr;
            if (!lightReady) ImGui::BeginDisabled();

            if (ImGui::Checkbox("Anti Darkness (ChangeLighting)", &antiDarkness) && lightReady)
            {
                if (antiDarkness)
                {
                    BYTE patch[] = { 0xC3 };
                    WritePatch(procID, changeLightAddr, patch, 1);
                }
                else
                {
                    // Restore original first 5 bytes of the function
                    WritePatch(procID, changeLightAddr,
                        changeLightOriginal.data(), changeLightOriginal.size());
                }
            }

            if (!lightReady) ImGui::EndDisabled();
        }

        ImGui::Spacing();

        // No Fog of War — fully independent checkbox
        {
            bool fogReady = hProcess && fogOfWarAddr;
            if (!fogReady) ImGui::BeginDisabled();

            if (ImGui::Checkbox("No Fog of War", &noFogOfWar) && fogReady)
            {
                if (noFogOfWar)
                {
                    BYTE patch[] = { 0xC3 };
                    WritePatch(procID, fogOfWarAddr, patch, 1);
                }
                else
                {
                    // Restore original first 2 bytes of the function
                    WritePatch(procID, fogOfWarAddr,
                        fogOfWarOriginal.data(), fogOfWarOriginal.size());
                }
            }

            if (!fogReady) ImGui::EndDisabled();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(1, 0.8f, 0.1f, 1),
            "Attach first before enabling modules");

        if (!hProcess || (!changeLightAddr && !fogOfWarAddr && !fishingAddr))
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
                "Make sure game is running UNPATCHED before scanning.");
        }

        ImGui::EndChild();

        ImGui::Columns(1);
        ImGui::End();

        ImGui::Render();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.02f, 0.02f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Cleanup
    if (hProcess) CloseHandle(hProcess);
    glDeleteTextures(1, &logoTexture);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

// ─────────────────────────────────────────────
//  GetProcId
// ─────────────────────────────────────────────
DWORD GetProcId(const char* procName)
{
    PROCESSENTRY32 pe32{ sizeof(PROCESSENTRY32) };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

    if (Process32First(hSnap, &pe32))
    {
        do {
            if (_stricmp(pe32.szExeFile, procName) == 0)
            {
                CloseHandle(hSnap);
                return pe32.th32ProcessID;
            }
        } while (Process32Next(hSnap, &pe32));
    }

    CloseHandle(hSnap);
    return 0;
}

// ─────────────────────────────────────────────
//  GetModuleBase
//  Returns the base address (and optionally size)
//  of a named module inside the target process.
// ─────────────────────────────────────────────
uintptr_t GetModuleBase(DWORD pid, const char* moduleName, SIZE_T* outSize)
{
    MODULEENTRY32 me32{ sizeof(MODULEENTRY32) };
    HANDLE hSnap = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);

    if (hSnap == INVALID_HANDLE_VALUE) return 0;

    if (Module32First(hSnap, &me32))
    {
        do {
            if (_stricmp(me32.szModule, moduleName) == 0)
            {
                CloseHandle(hSnap);
                if (outSize) *outSize = me32.modBaseSize;
                return (uintptr_t)me32.modBaseAddr;
            }
        } while (Module32Next(hSnap, &me32));
    }

    CloseHandle(hSnap);
    return 0;
}

// ─────────────────────────────────────────────
//  FindPatternInModule
//  Scans only within the named DLL's memory
//  range — faster and avoids false positives.
//  Walks pages individually so it handles
//  large DLLs (GameAssembly can be 200+ MB)
//  without a single enormous allocation.
// ─────────────────────────────────────────────
uintptr_t FindPatternInModule(HANDLE hProc, DWORD pid, const char* moduleName,
    const std::vector<BYTE>& pattern, const std::string& mask)
{
    if (pattern.size() != mask.size())
    {
        std::cout << "[-] Pattern/mask size mismatch for module scan! pattern="
            << pattern.size() << " mask=" << mask.size() << std::endl;
        return 0;
    }

    SIZE_T    moduleSize = 0;
    uintptr_t moduleBase = GetModuleBase(pid, moduleName, &moduleSize);

    if (!moduleBase)
    {
        std::cout << "[-] Module not found: " << moduleName << std::endl;
        return 0;
    }

    std::cout << "[*] Scanning " << moduleName
        << " base=0x" << std::hex << moduleBase
        << " size=0x" << moduleSize << std::endl;

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = moduleBase;
    uintptr_t endAddr = moduleBase + moduleSize;

    while (addr < endAddr)
    {
        if (VirtualQueryEx(hProc, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;

        // Only scan committed, executable pages within this module
        bool isExecutable = (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0;
        bool isCommitted = (mbi.State == MEM_COMMIT);

        if (isCommitted && isExecutable)
        {
            // Clamp region to module bounds
            uintptr_t regionEnd = addr + mbi.RegionSize;
            SIZE_T    regionSize = (regionEnd > endAddr)
                ? (SIZE_T)(endAddr - addr)
                : mbi.RegionSize;

            std::vector<BYTE> buffer(regionSize);
            SIZE_T bytesRead = 0;

            if (ReadProcessMemory(hProc, (LPCVOID)addr, buffer.data(), regionSize, &bytesRead)
                && bytesRead >= pattern.size())
            {
                for (size_t i = 0; i <= bytesRead - pattern.size(); ++i)
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

// ─────────────────────────────────────────────
//  FindPattern  (full address-space scan,
//  kept for features not tied to a specific DLL)
// ─────────────────────────────────────────────
uintptr_t FindPattern(HANDLE hProc,
    const std::vector<BYTE>& pattern, const std::string& mask)
{
    if (pattern.size() != mask.size())
    {
        std::cout << "[-] Pattern/mask size mismatch! pattern="
            << pattern.size() << " mask=" << mask.size() << std::endl;
        return 0;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);

    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = (uintptr_t)si.lpMinimumApplicationAddress;

    while (addr < (uintptr_t)si.lpMaximumApplicationAddress)
    {
        if (VirtualQueryEx(hProc, (LPCVOID)addr, &mbi, sizeof(mbi)) == 0) break;

        if (mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        {
            std::vector<BYTE> buffer(mbi.RegionSize);
            SIZE_T bytesRead = 0;

            if (ReadProcessMemory(hProc, (LPCVOID)addr,
                buffer.data(), mbi.RegionSize, &bytesRead)
                && bytesRead >= pattern.size())
            {
                for (size_t i = 0; i <= bytesRead - pattern.size(); ++i)
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