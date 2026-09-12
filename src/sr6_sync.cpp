#include "sr6_sync.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include "motion_input.h"
#include "orgasm_state.h"

enum class OutputTarget
{
    SerialSr6 = 0,
    Intiface = 1
};

enum class IntifaceCommandType
{
    HwPositionWithDuration = 0,
    Position = 1,
    Vibrate = 2,
    Oscillate = 3,
    Constrict = 4,
    Rotate = 5
};

constexpr int kButtplugProtocolVersionMajor = 4;
constexpr int kButtplugProtocolVersionMinor = 0;
constexpr DWORD kIntifaceScanDurationMs = 1000;
constexpr DWORD kIntifacePostScanReceiveMs = 500;
constexpr DWORD kIntifaceHandshakeTimeoutMs = 2000;
constexpr int kMinUpdateMs = 8;        // 125 Hz maximum; safe headroom for six-axis at 115200 baud.
constexpr int kMaxUpdateMs = 500;      // 2 Hz minimum.
constexpr int kMaxPatternHz = 100;

enum class UiLanguage
{
    English = 0,
    Chinese = 1
};

struct Sr6Config
{
    bool enabled = true;
    bool autoConnect = false;
    bool invertX = false;
    bool parkOnNotInserted = true;
    bool enableSixAxis = true;
    int baudRate = 115200;
    int updateMs = 20;
    int rampMs = 18;
    bool activeOscillationSuppression = true;
    float oscillationDeadband = 0.5f;
    int guiHotkey = 'H';
    int consoleHotkey = VK_F1;
    bool guiVisible = true;
    bool consoleVisible = true;
    UiLanguage language = UiLanguage::English;
    OutputTarget outputTarget = OutputTarget::SerialSr6;
    bool orgasmSync = true;
    int orgasmClimaxHz = 10;
    int orgasmAfterHz = 1;
    int orgasmLMin = 400;
    int orgasmLMax = 600;
    int orgasmRMin = 400;
    int orgasmRMax = 600;
    std::string intifaceUrl = "ws://localhost:12345";
    int intifaceDeviceIndex = 0;
    std::string portName;
};

static std::mutex g_sr6ConfigMutex;
static Sr6Config g_sr6Config;

static std::mutex g_sr6StatusMutex;
static std::string g_sr6Status = "SR6 idle";

static std::mutex g_sr6CommandMutex;
static std::string g_sr6LastCommand = "(none)";
static std::string g_sr6CommandHistory;

struct IntifaceDeviceInfo
{
    int index = 0;
    std::string name;
    struct OutputFeature
    {
        bool supported = false;
        int featureIndex = 0;
        int valueMin = 0;
        int valueMax = 100;
        int durationMin = 1;
        int durationMax = 5000;
    };

    OutputFeature hwPositionWithDuration;
    OutputFeature position;
    OutputFeature vibrate;
    OutputFeature oscillate;
    OutputFeature constrict;
    OutputFeature rotate;
};

static std::mutex g_intifaceDeviceMutex;
static std::vector<IntifaceDeviceInfo> g_intifaceDevices;

static std::atomic<bool> g_sr6Connected{ false };
static std::atomic<bool> g_sr6ConnectionInProgress{ false };
static std::atomic<bool> g_sr6ConnectRequested{ false };
static std::atomic<bool> g_sr6DisconnectRequested{ false };
static std::atomic<bool> g_sr6Running{ false };
static std::atomic<bool> g_intifaceGuiScanRunning{ false };
static std::atomic<bool> g_intifaceGuiTestRunning{ false };
static std::atomic<bool> g_intifaceWorkerScanRequested{ false };
static std::atomic<bool> g_intifaceWorkerTestRequested{ false };

static HWND g_guiWindow = nullptr;
static HWND g_consoleWindow = nullptr;
static std::atomic<bool> g_consoleVisible{ true };
static HBRUSH g_commandEditBrush = nullptr;
static HBRUSH g_guiBgBrush = nullptr;
static HBRUSH g_editBrush = nullptr;
static HFONT g_guiFont = nullptr;

constexpr COLORREF kGuiBgColor = RGB(246, 248, 250);
constexpr COLORREF kGuiTextColor = RGB(36, 41, 47);
constexpr COLORREF kGuiMutedTextColor = RGB(87, 96, 106);
constexpr COLORREF kGuiEditBgColor = RGB(255, 255, 255);
constexpr COLORREF kGuiCommandBgColor = RGB(255, 255, 255);
constexpr COLORREF kGuiCommandTextColor = RGB(31, 111, 235);
constexpr COLORREF kGuiAccentColor = RGB(9, 105, 218);
constexpr COLORREF kGuiBorderColor = RGB(208, 215, 222);

constexpr UINT WM_SR6_TOGGLE_GUI = WM_APP + 100;
constexpr UINT WM_SR6_QUIT = WM_APP + 101;
constexpr UINT WM_SR6_COMMAND_HISTORY_CHANGED = WM_APP + 102;
constexpr UINT WM_SR6_STATUS_CHANGED = WM_APP + 103;
constexpr UINT WM_SR6_INTIFACE_SCAN_DONE = WM_APP + 104;
constexpr UINT WM_SR6_INTIFACE_TEST_DONE = WM_APP + 105;

static std::string GetIniPath()
{
    char exePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return "MokaLoveReliveMotionSync.ini";

    std::string path(exePath, len);
    const size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos)
        return "MokaLoveReliveMotionSync.ini";

    return path.substr(0, slash + 1) + "MokaLoveReliveMotionSync.ini";
}

static int ClampInt(int value, int minValue, int maxValue)
{
    return std::max(minValue, std::min(maxValue, value));
}

static int MinimumSmoothRampMs(int updateMs)
{
    // Leave roughly ten percent for scheduling/serial jitter, but never let
    // the device finish so early that it visibly waits for the next command.
    updateMs = ClampInt(updateMs, kMinUpdateMs, kMaxUpdateMs);
    const int arrivalMarginMs = ClampInt((updateMs + 9) / 10, 1, 10);
    return std::max(0, updateMs - arrivalMarginMs);
}

static int NormalizeRampMs(int rampMs, int updateMs)
{
    updateMs = ClampInt(updateMs, kMinUpdateMs, kMaxUpdateMs);
    return ClampInt(rampMs, MinimumSmoothRampMs(updateMs), updateMs);
}

static OutputTarget ClampOutputTarget(int value)
{
    return value == static_cast<int>(OutputTarget::Intiface) ? OutputTarget::Intiface : OutputTarget::SerialSr6;
}

static UiLanguage ClampUiLanguage(int value)
{
    return value == static_cast<int>(UiLanguage::Chinese) ? UiLanguage::Chinese : UiLanguage::English;
}

static bool IsChinese(const Sr6Config& cfg)
{
    return cfg.language == UiLanguage::Chinese;
}

static const char* OutputTargetName(OutputTarget target)
{
    return target == OutputTarget::Intiface ? "Intiface Central" : "Serial SR6";
}

static const char* IntifaceCommandTypeName(IntifaceCommandType type)
{
    switch (type)
    {
    case IntifaceCommandType::Position:
        return "Position";
    case IntifaceCommandType::Vibrate:
        return "Vibrate";
    case IntifaceCommandType::Oscillate:
        return "Oscillate";
    case IntifaceCommandType::Constrict:
        return "Constrict";
    case IntifaceCommandType::Rotate:
        return "Rotate";
    default:
        return "HwPositionWithDuration";
    }
}

static float ClampFloat(float value, float minValue, float maxValue)
{
    return std::max(minValue, std::min(maxValue, value));
}

static Sr6Config GetSr6Config()
{
    std::lock_guard<std::mutex> lock(g_sr6ConfigMutex);
    return g_sr6Config;
}

static void SetSr6Status(const std::string& status, bool print = true)
{
    {
        std::lock_guard<std::mutex> lock(g_sr6StatusMutex);
        g_sr6Status = status;
    }

    HWND hwnd = g_guiWindow;
    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_STATUS_CHANGED, 0, 0);

    if (print)
        std::cout << "[SR6] " << status << std::endl;
}

static std::string GetSr6Status()
{
    std::lock_guard<std::mutex> lock(g_sr6StatusMutex);
    return g_sr6Status;
}

static const char* HotkeyName(int vk);

static float ReadIniFloat(
    const char* section,
    const char* key,
    float fallback,
    const std::string& iniPath)
{
    char fallbackText[32]{};
    char value[64]{};
    std::snprintf(fallbackText, sizeof(fallbackText), "%.3f", fallback);
    GetPrivateProfileStringA(
        section, key, fallbackText, value,
        static_cast<DWORD>(sizeof(value)), iniPath.c_str());
    char* end = nullptr;
    const float parsed = std::strtof(value, &end);
    return end && end != value && std::isfinite(parsed) ? parsed : fallback;
}

static void LoadSr6Config()
{
    const std::string iniPath = GetIniPath();
    char portBuffer[64]{};
    char intifaceUrlBuffer[256]{};
    GetPrivateProfileStringA("SR6", "Port", "", portBuffer, static_cast<DWORD>(sizeof(portBuffer)), iniPath.c_str());
    GetPrivateProfileStringA("SR6", "IntifaceUrl", "ws://localhost:12345", intifaceUrlBuffer, static_cast<DWORD>(sizeof(intifaceUrlBuffer)), iniPath.c_str());

    Sr6Config cfg;
    cfg.enabled = GetPrivateProfileIntA("SR6", "Enabled", cfg.enabled ? 1 : 0, iniPath.c_str()) != 0;
    cfg.autoConnect = GetPrivateProfileIntA("SR6", "AutoConnect", cfg.autoConnect ? 1 : 0, iniPath.c_str()) != 0;
    cfg.invertX = GetPrivateProfileIntA("SR6", "InvertX", cfg.invertX ? 1 : 0, iniPath.c_str()) != 0;
    cfg.parkOnNotInserted = GetPrivateProfileIntA("SR6", "ParkOnNotInserted", cfg.parkOnNotInserted ? 1 : 0, iniPath.c_str()) != 0;
    const bool legacyEnableR1 = GetPrivateProfileIntA("SR6", "EnableR1", cfg.enableSixAxis ? 1 : 0, iniPath.c_str()) != 0;
    cfg.enableSixAxis = GetPrivateProfileIntA("SR6", "EnableSixAxis", legacyEnableR1 ? 1 : 0, iniPath.c_str()) != 0;
    cfg.baudRate = ClampInt(GetPrivateProfileIntA("SR6", "BaudRate", cfg.baudRate, iniPath.c_str()), 9600, 1000000);
    cfg.updateMs = ClampInt(GetPrivateProfileIntA("SR6", "UpdateMs", cfg.updateMs, iniPath.c_str()), kMinUpdateMs, kMaxUpdateMs);
    cfg.rampMs = NormalizeRampMs(
        GetPrivateProfileIntA("SR6", "RampMs", cfg.rampMs, iniPath.c_str()),
        cfg.updateMs);
    cfg.activeOscillationSuppression = GetPrivateProfileIntA(
        "SR6", "ActiveOscillationSuppression",
        cfg.activeOscillationSuppression ? 1 : 0, iniPath.c_str()) != 0;
    cfg.oscillationDeadband = ClampFloat(
        ReadIniFloat("SR6", "OscillationDeadband", cfg.oscillationDeadband, iniPath),
        0.0f,
        0.5f);
    cfg.guiHotkey = GetPrivateProfileIntA("SR6", "GuiHotkey", cfg.guiHotkey, iniPath.c_str());
    cfg.consoleHotkey = GetPrivateProfileIntA("SR6", "ConsoleHotkey", cfg.consoleHotkey, iniPath.c_str());
    cfg.guiVisible = GetPrivateProfileIntA("SR6", "GuiVisible", cfg.guiVisible ? 1 : 0, iniPath.c_str()) != 0;
    cfg.consoleVisible = GetPrivateProfileIntA("SR6", "ConsoleVisible", cfg.consoleVisible ? 1 : 0, iniPath.c_str()) != 0;
    cfg.language = ClampUiLanguage(GetPrivateProfileIntA("SR6", "Language", static_cast<int>(cfg.language), iniPath.c_str()));
    cfg.outputTarget = ClampOutputTarget(GetPrivateProfileIntA("SR6", "OutputTarget", static_cast<int>(cfg.outputTarget), iniPath.c_str()));
    cfg.orgasmSync = GetPrivateProfileIntA("SR6", "OrgasmSync", cfg.orgasmSync ? 1 : 0, iniPath.c_str()) != 0;
    cfg.orgasmClimaxHz = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmClimaxHz", cfg.orgasmClimaxHz, iniPath.c_str()), 1, kMaxPatternHz);
    cfg.orgasmAfterHz = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmAfterHz", cfg.orgasmAfterHz, iniPath.c_str()), 1, kMaxPatternHz);
    cfg.orgasmLMin = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmLMin", cfg.orgasmLMin, iniPath.c_str()), 0, 999);
    cfg.orgasmLMax = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmLMax", cfg.orgasmLMax, iniPath.c_str()), 0, 999);
    cfg.orgasmRMin = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmRMin", cfg.orgasmRMin, iniPath.c_str()), 0, 999);
    cfg.orgasmRMax = ClampInt(GetPrivateProfileIntA("SR6", "OrgasmRMax", cfg.orgasmRMax, iniPath.c_str()), 0, 999);
    cfg.intifaceUrl = intifaceUrlBuffer;
    cfg.intifaceDeviceIndex = ClampInt(GetPrivateProfileIntA("SR6", "IntifaceDeviceIndex", cfg.intifaceDeviceIndex, iniPath.c_str()), 0, 99);
    if (cfg.orgasmLMin > cfg.orgasmLMax)
        std::swap(cfg.orgasmLMin, cfg.orgasmLMax);
    if (cfg.orgasmRMin > cfg.orgasmRMax)
        std::swap(cfg.orgasmRMin, cfg.orgasmRMax);
    cfg.portName = portBuffer;

    {
        std::lock_guard<std::mutex> lock(g_sr6ConfigMutex);
        g_sr6Config = cfg;
    }

    std::cout << "[SR6] config loaded: " << iniPath
              << " | port=" << (cfg.portName.empty() ? "(none)" : cfg.portName)
              << " | output=" << OutputTargetName(cfg.outputTarget)
              << " | autoConnect=" << (cfg.autoConnect ? "true" : "false")
              << " | updateMs=" << cfg.updateMs
              << " | rampMs=" << cfg.rampMs
              << " | oscillationSuppression=" << (cfg.activeOscillationSuppression ? "true" : "false")
              << " | oscillationDeadband=" << cfg.oscillationDeadband
              << " | sixAxis=" << (cfg.enableSixAxis ? "true" : "false")
              << " | orgasmSync=" << (cfg.orgasmSync ? "true" : "false")
              << " | climaxHz=" << cfg.orgasmClimaxHz
              << " | afterHz=" << cfg.orgasmAfterHz
              << " | guiHotkey=" << HotkeyName(cfg.guiHotkey)
              << " | consoleHotkey=" << HotkeyName(cfg.consoleHotkey)
              << " | guiVisible=" << (cfg.guiVisible ? "true" : "false")
              << " | consoleVisible=" << (cfg.consoleVisible ? "true" : "false")
              << " | language=" << (IsChinese(cfg) ? "Chinese" : "English")
              << std::endl;
}

static void SaveSr6Config(const Sr6Config& cfg)
{
    const std::string iniPath = GetIniPath();
    char value[64]{};

    WritePrivateProfileStringA("SR6", "Enabled", cfg.enabled ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "AutoConnect", cfg.autoConnect ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "InvertX", cfg.invertX ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "ParkOnNotInserted", cfg.parkOnNotInserted ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "EnableSixAxis", cfg.enableSixAxis ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "EnableR1", cfg.enableSixAxis ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "Port", cfg.portName.c_str(), iniPath.c_str());

    std::snprintf(value, sizeof(value), "%d", cfg.baudRate);
    WritePrivateProfileStringA("SR6", "BaudRate", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.updateMs);
    WritePrivateProfileStringA("SR6", "UpdateMs", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.rampMs);
    WritePrivateProfileStringA("SR6", "RampMs", value, iniPath.c_str());
    WritePrivateProfileStringA(
        "SR6", "ActiveOscillationSuppression",
        cfg.activeOscillationSuppression ? "1" : "0", iniPath.c_str());
    std::snprintf(value, sizeof(value), "%.3f", cfg.oscillationDeadband);
    WritePrivateProfileStringA("SR6", "OscillationDeadband", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.guiHotkey);
    WritePrivateProfileStringA("SR6", "GuiHotkey", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.consoleHotkey);
    WritePrivateProfileStringA("SR6", "ConsoleHotkey", value, iniPath.c_str());
    WritePrivateProfileStringA("SR6", "GuiVisible", cfg.guiVisible ? "1" : "0", iniPath.c_str());
    WritePrivateProfileStringA("SR6", "ConsoleVisible", cfg.consoleVisible ? "1" : "0", iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", static_cast<int>(cfg.language));
    WritePrivateProfileStringA("SR6", "Language", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", static_cast<int>(cfg.outputTarget));
    WritePrivateProfileStringA("SR6", "OutputTarget", value, iniPath.c_str());
    WritePrivateProfileStringA("SR6", "OrgasmSync", cfg.orgasmSync ? "1" : "0", iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmClimaxHz);
    WritePrivateProfileStringA("SR6", "OrgasmClimaxHz", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmAfterHz);
    WritePrivateProfileStringA("SR6", "OrgasmAfterHz", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmLMin);
    WritePrivateProfileStringA("SR6", "OrgasmLMin", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmLMax);
    WritePrivateProfileStringA("SR6", "OrgasmLMax", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmRMin);
    WritePrivateProfileStringA("SR6", "OrgasmRMin", value, iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.orgasmRMax);
    WritePrivateProfileStringA("SR6", "OrgasmRMax", value, iniPath.c_str());
    WritePrivateProfileStringA("SR6", "IntifaceUrl", cfg.intifaceUrl.c_str(), iniPath.c_str());
    std::snprintf(value, sizeof(value), "%d", cfg.intifaceDeviceIndex);
    WritePrivateProfileStringA("SR6", "IntifaceDeviceIndex", value, iniPath.c_str());
}

static void UpdateSr6Config(const Sr6Config& cfg, bool saveToDisk = true)
{
    Sr6Config normalized = cfg;
    normalized.updateMs = ClampInt(normalized.updateMs, kMinUpdateMs, kMaxUpdateMs);
    normalized.rampMs = NormalizeRampMs(normalized.rampMs, normalized.updateMs);
    normalized.oscillationDeadband = ClampFloat(normalized.oscillationDeadband, 0.0f, 0.5f);
    normalized.baudRate = ClampInt(normalized.baudRate, 9600, 1000000);
    normalized.language = ClampUiLanguage(static_cast<int>(normalized.language));
    normalized.outputTarget = ClampOutputTarget(static_cast<int>(normalized.outputTarget));
    normalized.orgasmClimaxHz = ClampInt(normalized.orgasmClimaxHz, 1, kMaxPatternHz);
    normalized.orgasmAfterHz = ClampInt(normalized.orgasmAfterHz, 1, kMaxPatternHz);
    normalized.orgasmLMin = ClampInt(normalized.orgasmLMin, 0, 999);
    normalized.orgasmLMax = ClampInt(normalized.orgasmLMax, 0, 999);
    normalized.orgasmRMin = ClampInt(normalized.orgasmRMin, 0, 999);
    normalized.orgasmRMax = ClampInt(normalized.orgasmRMax, 0, 999);
    if (normalized.orgasmLMin > normalized.orgasmLMax)
        std::swap(normalized.orgasmLMin, normalized.orgasmLMax);
    if (normalized.orgasmRMin > normalized.orgasmRMax)
        std::swap(normalized.orgasmRMin, normalized.orgasmRMax);
    normalized.intifaceDeviceIndex = ClampInt(normalized.intifaceDeviceIndex, 0, 99);
    if (normalized.intifaceUrl.empty())
        normalized.intifaceUrl = "ws://localhost:12345";

    {
        std::lock_guard<std::mutex> lock(g_sr6ConfigMutex);
        g_sr6Config = normalized;
    }

    if (saveToDisk)
        SaveSr6Config(normalized);
}

static void SaveVisibilityState(bool guiVisible, bool consoleVisible)
{
    Sr6Config cfg = GetSr6Config();
    cfg.guiVisible = guiVisible;
    cfg.consoleVisible = consoleVisible;
    UpdateSr6Config(cfg);
}

static std::vector<std::string> EnumerateSerialPorts()
{
    std::vector<std::string> ports;
    char target[512]{};

    for (int i = 1; i <= 256; ++i)
    {
        char name[16]{};
        std::snprintf(name, sizeof(name), "COM%d", i);
        if (QueryDosDeviceA(name, target, static_cast<DWORD>(sizeof(target))) != 0)
            ports.push_back(name);
    }

    return ports;
}

static std::string SerialDevicePath(const std::string& portName)
{
    if (portName.rfind("\\\\.\\", 0) == 0)
        return portName;
    return "\\\\.\\" + portName;
}

static bool WriteSerialLine(HANDLE serial, const std::string& line)
{
    if (serial == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(serial, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    return ok && written == line.size();
}

static std::string CommandForDisplay(const std::string& line)
{
    std::string out = line;
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    return out.empty() ? "(empty)" : out;
}

static void RecordSr6Command(const std::string& line)
{
    SYSTEMTIME now{};
    GetLocalTime(&now);

    const std::string command = CommandForDisplay(line);
    char prefix[64]{};
    std::snprintf(
        prefix,
        sizeof(prefix),
        "%02u:%02u:%02u.%03u  ",
        now.wHour,
        now.wMinute,
        now.wSecond,
        now.wMilliseconds);

    std::lock_guard<std::mutex> lock(g_sr6CommandMutex);
    g_sr6LastCommand = command;
    g_sr6CommandHistory += prefix;
    g_sr6CommandHistory += command;
    g_sr6CommandHistory += "\r\n";

    constexpr size_t MaxHistoryBytes = 8192;
    if (g_sr6CommandHistory.size() > MaxHistoryBytes)
    {
        const size_t trimTo = g_sr6CommandHistory.size() - MaxHistoryBytes;
        const size_t nextLine = g_sr6CommandHistory.find("\r\n", trimTo);
        if (nextLine != std::string::npos)
            g_sr6CommandHistory.erase(0, nextLine + 2);
        else
            g_sr6CommandHistory.erase(0, trimTo);
    }

    HWND hwnd = g_guiWindow;
    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_COMMAND_HISTORY_CHANGED, 0, 0);
}

static bool SendSr6Command(HANDLE serial, const std::string& line)
{
    if (!WriteSerialLine(serial, line))
        return false;

    RecordSr6Command(line);
    return true;
}

struct IntifaceConnection
{
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    HINTERNET websocket = nullptr;
    HANDLE receiveThread = nullptr;
    volatile LONG receiveStop = 0;
    unsigned int nextId = 1;
    unsigned int maxPingTimeMs = 0;
    unsigned int protocolVersionMajor = 0;
    unsigned int protocolVersionMinor = 0;
    unsigned long long lastPingTick = 0;
};

static DWORD WINAPI IntifaceReceiveThread(LPVOID param);
static bool StartIntifaceReceiver(IntifaceConnection& connection);
static void StopIntifaceReceiver(IntifaceConnection& connection);

static std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return L"";

    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 0)
        return L"";

    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &out[0], needed);
    return out;
}

static bool ParseWsUrl(const std::string& url, bool& secure, std::wstring& host, INTERNET_PORT& port, std::wstring& path)
{
    std::string work = url.empty() ? "ws://localhost:12345" : url;
    secure = false;

    const std::string wsPrefix = "ws://";
    const std::string wssPrefix = "wss://";
    if (work.rfind(wsPrefix, 0) == 0)
    {
        work.erase(0, wsPrefix.size());
        port = 80;
    }
    else if (work.rfind(wssPrefix, 0) == 0)
    {
        work.erase(0, wssPrefix.size());
        secure = true;
        port = 443;
    }
    else
    {
        port = 80;
    }

    const size_t slash = work.find('/');
    std::string hostPort = slash == std::string::npos ? work : work.substr(0, slash);
    path = Utf8ToWide(slash == std::string::npos ? "/" : work.substr(slash));

    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos)
    {
        const std::string portText = hostPort.substr(colon + 1);
        hostPort = hostPort.substr(0, colon);
        port = static_cast<INTERNET_PORT>(ClampInt(std::atoi(portText.c_str()), 1, 65535));
    }

    host = Utf8ToWide(hostPort);
    return !host.empty() && !path.empty();
}

static void AddUniqueUrl(std::vector<std::string>& urls, const std::string& url)
{
    if (url.empty())
        return;

    for (size_t i = 0; i < urls.size(); ++i)
    {
        if (_stricmp(urls[i].c_str(), url.c_str()) == 0)
            return;
    }

    urls.push_back(url);
}

static bool ReplaceFirst(std::string& text, const char* from, const char* to)
{
    const size_t pos = text.find(from);
    if (pos == std::string::npos)
        return false;

    text.replace(pos, std::strlen(from), to);
    return true;
}

static std::vector<std::string> BuildIntifaceUrlCandidates(const std::string& configuredUrl)
{
    std::vector<std::string> urls;
    const std::string baseUrl = configuredUrl.empty() ? "ws://localhost:12345" : configuredUrl;
    AddUniqueUrl(urls, baseUrl);

    std::string ipv4Url = baseUrl;
    if (ReplaceFirst(ipv4Url, "://localhost", "://127.0.0.1"))
        AddUniqueUrl(urls, ipv4Url);

    std::string localhostUrl = baseUrl;
    if (ReplaceFirst(localhostUrl, "://127.0.0.1", "://localhost"))
        AddUniqueUrl(urls, localhostUrl);

    return urls;
}

static bool StartIntifaceReceiver(IntifaceConnection& connection)
{
    if (!connection.websocket || connection.receiveThread)
        return false;

    InterlockedExchange(&connection.receiveStop, 0);
    HANDLE thread = CreateThread(nullptr, 0, IntifaceReceiveThread, &connection, 0, nullptr);
    if (!thread)
        return false;

    connection.receiveThread = thread;
    return true;
}

static void StopIntifaceReceiver(IntifaceConnection& connection)
{
    if (!connection.receiveThread)
        return;

    InterlockedExchange(&connection.receiveStop, 1);
    if (connection.websocket)
    {
        HINTERNET websocket = connection.websocket;
        connection.websocket = nullptr;
        WinHttpCloseHandle(websocket);
    }

    WaitForSingleObject(connection.receiveThread, 1000);
    CloseHandle(connection.receiveThread);
    connection.receiveThread = nullptr;
}

static void CloseIntifaceConnection(IntifaceConnection& connection)
{
    StopIntifaceReceiver(connection);

    if (connection.websocket)
    {
        WinHttpWebSocketClose(connection.websocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(connection.websocket);
    }
    if (connection.request)
        WinHttpCloseHandle(connection.request);
    if (connection.connect)
        WinHttpCloseHandle(connection.connect);
    if (connection.session)
        WinHttpCloseHandle(connection.session);

    connection = IntifaceConnection{};
}

static bool SendIntifaceJson(IntifaceConnection& connection, const std::string& json, bool recordCommand = true)
{
    if (!connection.websocket)
        return false;

    const DWORD result = WinHttpWebSocketSend(
        connection.websocket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(json.data()),
        static_cast<DWORD>(json.size()));
    if (result != NO_ERROR)
        return false;

    if (recordCommand)
        RecordSr6Command(json);
    return true;
}

static std::string MakeIntifaceServerInfo(unsigned int id)
{
    std::ostringstream oss;
    oss << "[{\"RequestServerInfo\":{\"Id\":" << id
        << ",\"ClientName\":\"MocaLoveRelive Motion Sync\""
        << ",\"ProtocolVersionMajor\":" << kButtplugProtocolVersionMajor
        << ",\"ProtocolVersionMinor\":" << kButtplugProtocolVersionMinor
        << "}}]";
    return oss.str();
}

static std::string MakeIntifaceSimpleMessage(const char* name, unsigned int id)
{
    std::ostringstream oss;
    oss << "[{\"" << name << "\":{\"Id\":" << id << "}}]";
    return oss.str();
}

static std::string MakeIntifacePing(unsigned int id)
{
    std::ostringstream oss;
    oss << "[{\"Ping\":{\"Id\":" << id << "}}]";
    return oss.str();
}

static std::string MakeIntifaceStop(unsigned int id, int deviceIndex = -1)
{
    std::ostringstream oss;
    oss << "[{\"StopCmd\":{\"Id\":" << id;
    if (deviceIndex >= 0)
        oss << ",\"DeviceIndex\":" << deviceIndex;
    oss << ",\"Inputs\":true,\"Outputs\":true}}]";
    return oss.str();
}

static bool HasOutputFeature(const IntifaceDeviceInfo::OutputFeature& feature)
{
    return feature.supported;
}

static void MergeOutputFeature(
    IntifaceDeviceInfo::OutputFeature& target,
    const IntifaceDeviceInfo::OutputFeature& source)
{
    if (!target.supported && source.supported)
        target = source;
}

static void MergeIntifaceDevice(const IntifaceDeviceInfo& info)
{
    std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
    for (size_t i = 0; i < g_intifaceDevices.size(); ++i)
    {
        if (g_intifaceDevices[i].index == info.index)
        {
            g_intifaceDevices[i].name = info.name;
            MergeOutputFeature(g_intifaceDevices[i].hwPositionWithDuration, info.hwPositionWithDuration);
            MergeOutputFeature(g_intifaceDevices[i].position, info.position);
            MergeOutputFeature(g_intifaceDevices[i].vibrate, info.vibrate);
            MergeOutputFeature(g_intifaceDevices[i].oscillate, info.oscillate);
            MergeOutputFeature(g_intifaceDevices[i].constrict, info.constrict);
            MergeOutputFeature(g_intifaceDevices[i].rotate, info.rotate);
            return;
        }
    }

    g_intifaceDevices.push_back(info);
}

static size_t SkipJsonWhitespace(const std::string& text, size_t pos)
{
    while (pos < text.size() &&
        (text[pos] == ' ' || text[pos] == '\r' || text[pos] == '\n' || text[pos] == '\t'))
    {
        ++pos;
    }
    return pos;
}

static size_t FindJsonStringEnd(const std::string& text, size_t quotePos)
{
    bool escaped = false;
    for (size_t i = quotePos + 1; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (escaped)
        {
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
            return i;
    }
    return std::string::npos;
}

static size_t FindMatchingJsonBrace(const std::string& text, size_t openPos)
{
    if (openPos >= text.size() || text[openPos] != '{')
        return std::string::npos;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = openPos; i < text.size(); ++i)
    {
        const char ch = text[i];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0)
                return i;
        }
    }

    return std::string::npos;
}

static bool FindJsonKeyValueStart(const std::string& text, size_t start, const char* key, size_t& valueStart)
{
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = start;
    while ((pos = text.find(needle, pos)) != std::string::npos)
    {
        size_t cursor = SkipJsonWhitespace(text, pos + needle.size());
        if (cursor < text.size() && text[cursor] == ':')
        {
            valueStart = SkipJsonWhitespace(text, cursor + 1);
            return true;
        }
        pos += needle.size();
    }

    return false;
}

static bool ExtractJsonObjectForKey(
    const std::string& text,
    size_t start,
    const char* key,
    size_t& objectStart,
    size_t& objectEnd)
{
    size_t valueStart = 0;
    if (!FindJsonKeyValueStart(text, start, key, valueStart) ||
        valueStart >= text.size() ||
        text[valueStart] != '{')
    {
        return false;
    }

    const size_t end = FindMatchingJsonBrace(text, valueStart);
    if (end == std::string::npos)
        return false;

    objectStart = valueStart;
    objectEnd = end;
    return true;
}

static bool NextJsonObjectValue(
    const std::string& text,
    size_t objectEnd,
    size_t& pos,
    size_t& valueStart,
    size_t& valueEnd)
{
    while (pos < objectEnd)
    {
        const size_t keyStart = text.find('"', pos);
        if (keyStart == std::string::npos || keyStart >= objectEnd)
            return false;

        const size_t keyEnd = FindJsonStringEnd(text, keyStart);
        if (keyEnd == std::string::npos || keyEnd >= objectEnd)
            return false;

        size_t cursor = SkipJsonWhitespace(text, keyEnd + 1);
        if (cursor >= objectEnd || text[cursor] != ':')
        {
            pos = keyEnd + 1;
            continue;
        }

        cursor = SkipJsonWhitespace(text, cursor + 1);
        if (cursor < objectEnd && text[cursor] == '{')
        {
            const size_t end = FindMatchingJsonBrace(text, cursor);
            if (end == std::string::npos || end > objectEnd)
                return false;

            valueStart = cursor;
            valueEnd = end;
            pos = end + 1;
            return true;
        }

        pos = cursor + 1;
    }

    return false;
}

static std::string JsonStringValueNear(const std::string& text, size_t start, const char* key)
{
    size_t valueStart = 0;
    if (!FindJsonKeyValueStart(text, start, key, valueStart) ||
        valueStart >= text.size() ||
        text[valueStart] != '"')
    {
        return "";
    }

    const size_t valueEnd = FindJsonStringEnd(text, valueStart);
    if (valueEnd == std::string::npos)
        return "";
    return text.substr(valueStart + 1, valueEnd - valueStart - 1);
}

static bool JsonIntValueNear(const std::string& text, size_t start, const char* key, int& value)
{
    size_t valueStart = 0;
    if (!FindJsonKeyValueStart(text, start, key, valueStart))
        return false;

    char* end = nullptr;
    const long parsed = std::strtol(text.c_str() + valueStart, &end, 10);
    if (end == text.c_str() + valueStart)
        return false;

    value = static_cast<int>(parsed);
    return true;
}

static bool JsonRangeValueNear(const std::string& text, size_t start, const char* key, int& minValue, int& maxValue)
{
    size_t valueStart = 0;
    if (!FindJsonKeyValueStart(text, start, key, valueStart) ||
        valueStart >= text.size() ||
        text[valueStart] != '[')
    {
        return false;
    }

    const char* cursor = text.c_str() + valueStart + 1;
    char* end = nullptr;
    const long first = std::strtol(cursor, &end, 10);
    if (end == cursor)
        return false;

    cursor = end;
    while (*cursor && (*cursor == ' ' || *cursor == '\r' || *cursor == '\n' || *cursor == '\t' || *cursor == ','))
        ++cursor;

    const long second = std::strtol(cursor, &end, 10);
    if (end == cursor)
        return false;

    minValue = static_cast<int>(first);
    maxValue = static_cast<int>(second);
    if (maxValue < minValue)
        std::swap(minValue, maxValue);
    return true;
}

static void ReadIntifaceOutputFeature(
    const std::string& featureBlock,
    const char* outputType,
    int featureIndex,
    int defaultValueMax,
    int defaultDurationMax,
    IntifaceDeviceInfo::OutputFeature& outFeature)
{
    if (outFeature.supported)
        return;

    size_t outputStart = 0;
    size_t outputEnd = 0;
    if (!ExtractJsonObjectForKey(featureBlock, 0, outputType, outputStart, outputEnd))
        return;

    const std::string outputBlock = featureBlock.substr(outputStart, outputEnd - outputStart + 1);
    IntifaceDeviceInfo::OutputFeature feature{};
    feature.supported = true;
    feature.featureIndex = featureIndex;
    feature.valueMin = 0;
    feature.valueMax = defaultValueMax;
    feature.durationMin = 1;
    feature.durationMax = defaultDurationMax;
    JsonRangeValueNear(outputBlock, 0, "Value", feature.valueMin, feature.valueMax);
    JsonRangeValueNear(outputBlock, 0, "Duration", feature.durationMin, feature.durationMax);
    feature.valueMax = std::max(feature.valueMin, feature.valueMax);
    feature.durationMax = std::max(feature.durationMin, feature.durationMax);
    outFeature = feature;
}

static void ParseIntifaceV4Features(const std::string& deviceBlock, IntifaceDeviceInfo& device)
{
    size_t featuresStart = 0;
    size_t featuresEnd = 0;
    if (!ExtractJsonObjectForKey(deviceBlock, 0, "DeviceFeatures", featuresStart, featuresEnd))
        return;

    size_t pos = featuresStart + 1;
    size_t featureStart = 0;
    size_t featureEnd = 0;
    while (NextJsonObjectValue(deviceBlock, featuresEnd, pos, featureStart, featureEnd))
    {
        const std::string featureBlock = deviceBlock.substr(featureStart, featureEnd - featureStart + 1);
        int featureIndex = 0;
        if (!JsonIntValueNear(featureBlock, 0, "FeatureIndex", featureIndex))
            continue;

        ReadIntifaceOutputFeature(featureBlock, "HwPositionWithDuration", featureIndex, 100, 5000, device.hwPositionWithDuration);
        ReadIntifaceOutputFeature(featureBlock, "Position", featureIndex, 100, 1, device.position);
        ReadIntifaceOutputFeature(featureBlock, "Vibrate", featureIndex, 20, 1, device.vibrate);
        ReadIntifaceOutputFeature(featureBlock, "Oscillate", featureIndex, 20, 1, device.oscillate);
        ReadIntifaceOutputFeature(featureBlock, "Constrict", featureIndex, 20, 1, device.constrict);
        ReadIntifaceOutputFeature(featureBlock, "Rotate", featureIndex, 20, 1, device.rotate);
    }
}

static bool ParseIntifaceV4DevicesFromMessage(const std::string& message)
{
    size_t deviceListStart = 0;
    size_t deviceListEnd = 0;
    if (!ExtractJsonObjectForKey(message, 0, "DeviceList", deviceListStart, deviceListEnd))
        return false;

    const std::string deviceListBlock = message.substr(deviceListStart, deviceListEnd - deviceListStart + 1);
    size_t devicesStart = 0;
    size_t devicesEnd = 0;
    if (!ExtractJsonObjectForKey(deviceListBlock, 0, "Devices", devicesStart, devicesEnd))
        return false;

    std::vector<IntifaceDeviceInfo> devices;
    size_t pos = devicesStart + 1;
    size_t deviceStart = 0;
    size_t deviceEnd = 0;
    while (NextJsonObjectValue(deviceListBlock, devicesEnd, pos, deviceStart, deviceEnd))
    {
        const std::string deviceBlock = deviceListBlock.substr(deviceStart, deviceEnd - deviceStart + 1);
        IntifaceDeviceInfo device{};
        if (!JsonIntValueNear(deviceBlock, 0, "DeviceIndex", device.index))
            continue;

        device.name = JsonStringValueNear(deviceBlock, 0, "DeviceDisplayName");
        if (device.name.empty())
            device.name = JsonStringValueNear(deviceBlock, 0, "DeviceName");
        if (device.name.empty())
            device.name = "Device " + std::to_string(device.index);

        ParseIntifaceV4Features(deviceBlock, device);
        devices.push_back(device);
    }

    {
        std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
        g_intifaceDevices = devices;
    }
    return true;
}

static bool ParseIntifaceLegacyDevicesFromMessage(const std::string& message)
{
    bool foundDevice = false;
    size_t pos = 0;
    while ((pos = message.find("\"DeviceIndex\":", pos)) != std::string::npos)
    {
        int index = 0;
        if (JsonIntValueNear(message, pos, "DeviceIndex", index))
        {
            const size_t nextDevice = message.find("\"DeviceIndex\":", pos + 14);
            const std::string deviceBlock = nextDevice == std::string::npos
                ? message.substr(pos)
                : message.substr(pos, nextDevice - pos);
            std::string name = JsonStringValueNear(message, pos, "DeviceName");
            if (name.empty())
                name = JsonStringValueNear(message, pos, "DeviceDisplayName");

            const bool supportsLinear =
                deviceBlock.find("\"LinearCmd\"") != std::string::npos ||
                deviceBlock.find("\"PositionWithDuration\"") != std::string::npos;
            const bool supportsScalar =
                deviceBlock.find("\"ScalarCmd\"") != std::string::npos ||
                deviceBlock.find("\"Vibrate\"") != std::string::npos ||
                deviceBlock.find("\"Oscillate\"") != std::string::npos ||
                deviceBlock.find("\"Constrict\"") != std::string::npos ||
                deviceBlock.find("\"Inflate\"") != std::string::npos;
            const bool supportsRotate =
                deviceBlock.find("\"RotateCmd\"") != std::string::npos ||
                deviceBlock.find("\"Rotate\"") != std::string::npos;

            IntifaceDeviceInfo info{};
            info.index = index;
            info.name = name.empty() ? ("Device " + std::to_string(index)) : name;
            if (supportsLinear)
            {
                info.hwPositionWithDuration.supported = true;
                info.hwPositionWithDuration.featureIndex = 0;
                info.hwPositionWithDuration.valueMin = 0;
                info.hwPositionWithDuration.valueMax = 100;
                info.hwPositionWithDuration.durationMin = 1;
                info.hwPositionWithDuration.durationMax = 5000;
            }
            if (supportsScalar)
            {
                info.vibrate.supported = true;
                info.vibrate.featureIndex = 0;
                info.vibrate.valueMin = 0;
                info.vibrate.valueMax = 20;
            }
            if (supportsRotate)
            {
                info.rotate.supported = true;
                info.rotate.featureIndex = 0;
                info.rotate.valueMin = 0;
                info.rotate.valueMax = 20;
            }
            MergeIntifaceDevice(info);
            foundDevice = true;
        }
        pos += 14;
    }

    return foundDevice;
}

static bool ParseIntifaceDevicesFromMessage(const std::string& message)
{
    if (ParseIntifaceV4DevicesFromMessage(message))
        return true;
    return ParseIntifaceLegacyDevicesFromMessage(message);
}

static void ParseIntifaceServerInfoFromMessage(IntifaceConnection& connection, const std::string& message)
{
    int maxPingTime = 0;
    if (JsonIntValueNear(message, 0, "MaxPingTime", maxPingTime) && maxPingTime > 0)
        connection.maxPingTimeMs = static_cast<unsigned int>(ClampInt(maxPingTime, 1000, 60000));

    int protocolMajor = 0;
    if (JsonIntValueNear(message, 0, "ProtocolVersionMajor", protocolMajor) && protocolMajor >= 0)
        connection.protocolVersionMajor = static_cast<unsigned int>(protocolMajor);

    int protocolMinor = 0;
    if (JsonIntValueNear(message, 0, "ProtocolVersionMinor", protocolMinor) && protocolMinor >= 0)
        connection.protocolVersionMinor = static_cast<unsigned int>(protocolMinor);
}

static DWORD WINAPI IntifaceReceiveThread(LPVOID param)
{
    IntifaceConnection* connection = reinterpret_cast<IntifaceConnection*>(param);
    if (!connection)
        return 0;

    char buffer[8192]{};
    while (InterlockedCompareExchange(&connection->receiveStop, 0, 0) == 0)
    {
        HINTERNET websocket = connection->websocket;
        if (!websocket)
            break;

        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(websocket, buffer, sizeof(buffer) - 1, &bytesRead, &type);
        if (InterlockedCompareExchange(&connection->receiveStop, 0, 0) != 0)
            break;
        if (result == ERROR_WINHTTP_TIMEOUT)
            continue;
        if (result != NO_ERROR)
            break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;
        if (bytesRead == 0)
            continue;

        buffer[bytesRead] = '\0';
        ParseIntifaceServerInfoFromMessage(*connection, buffer);
        const bool devicesUpdated = ParseIntifaceDevicesFromMessage(buffer);
        if (devicesUpdated)
        {
            HWND hwnd = g_guiWindow;
            if (hwnd && IsWindow(hwnd))
                PostMessageA(hwnd, WM_SR6_INTIFACE_SCAN_DONE, 1, 0);
        }

    }

    return 0;
}

static void ReceiveIntifaceMessages(IntifaceConnection& connection, DWORD totalMs)
{
    if (!connection.websocket || totalMs == 0)
        return;

    DWORD timeout = std::max<DWORD>(1, std::min<DWORD>(100, totalMs));
    WinHttpSetOption(connection.websocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    const unsigned long long endTick = GetTickCount64() + totalMs;
    char buffer[4096]{};
    while (GetTickCount64() < endTick)
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(connection.websocket, buffer, sizeof(buffer) - 1, &bytesRead, &type);
        if (result == ERROR_WINHTTP_TIMEOUT)
            continue;
        if (result != NO_ERROR)
            break;
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
            break;
        if (bytesRead == 0)
            continue;

        buffer[bytesRead] = '\0';
        ParseIntifaceServerInfoFromMessage(connection, buffer);
        ParseIntifaceDevicesFromMessage(buffer);
    }
}

static bool WaitForIntifaceHandshake(IntifaceConnection& connection, std::string& error)
{
    if (!connection.websocket)
    {
        error = "Intiface websocket is not open";
        return false;
    }

    DWORD timeout = 100;
    WinHttpSetOption(connection.websocket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    const unsigned long long endTick = GetTickCount64() + kIntifaceHandshakeTimeoutMs;
    char buffer[8192]{};
    while (GetTickCount64() < endTick)
    {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(connection.websocket, buffer, sizeof(buffer) - 1, &bytesRead, &type);
        if (result == ERROR_WINHTTP_TIMEOUT)
            continue;
        if (result != NO_ERROR)
        {
            error = "Intiface handshake receive failed code=" + std::to_string(result);
            return false;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
        {
            error = "Intiface closed websocket during handshake";
            return false;
        }
        if (bytesRead == 0)
            continue;

        buffer[bytesRead] = '\0';
        ParseIntifaceServerInfoFromMessage(connection, buffer);
        ParseIntifaceDevicesFromMessage(buffer);

        if (std::strstr(buffer, "\"Error\""))
        {
            const std::string errorMessage = JsonStringValueNear(buffer, 0, "ErrorMessage");
            error = errorMessage.empty() ? "Intiface returned handshake error" : ("Intiface error: " + errorMessage);
            return false;
        }

        if (std::strstr(buffer, "\"ServerInfo\""))
        {
            if (connection.protocolVersionMajor != kButtplugProtocolVersionMajor)
            {
                error = "Intiface protocol mismatch: server="
                    + std::to_string(connection.protocolVersionMajor)
                    + "."
                    + std::to_string(connection.protocolVersionMinor)
                    + " client="
                    + std::to_string(kButtplugProtocolVersionMajor)
                    + "."
                    + std::to_string(kButtplugProtocolVersionMinor);
                return false;
            }
            return true;
        }
    }

    error = "Intiface handshake timed out waiting for ServerInfo";
    return false;
}

static void ServiceIntifaceConnection(IntifaceConnection& connection, DWORD receiveMs = 1)
{
    if (!connection.websocket)
        return;

    const unsigned long long now = GetTickCount64();
    if (connection.maxPingTimeMs > 0)
    {
        const unsigned int intervalMs = std::max(1000u, connection.maxPingTimeMs / 2);
        if (connection.lastPingTick == 0 || now - connection.lastPingTick >= intervalMs)
        {
            SendIntifaceJson(connection, MakeIntifacePing(connection.nextId++), false);
            connection.lastPingTick = now;
        }
    }
    (void)receiveMs;
}

static bool OpenIntifaceConnectionUrl(const std::string& url, IntifaceConnection& connection, std::string& error)
{
    CloseIntifaceConnection(connection);

    bool secure = false;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 80;
    if (!ParseWsUrl(url, secure, host, port, path))
    {
        error = "invalid Intiface URL";
        return false;
    }

    connection.session = WinHttpOpen(L"MocaLoveRelive Motion Sync/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!connection.session)
    {
        error = "WinHttpOpen failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }
    WinHttpSetTimeouts(connection.session, 3000, 3000, 3000, 3000);

    connection.connect = WinHttpConnect(connection.session, host.c_str(), port, 0);
    if (!connection.connect)
    {
        error = "WinHttpConnect failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }

    const DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    connection.request = WinHttpOpenRequest(connection.connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!connection.request)
    {
        error = "WinHttpOpenRequest failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }

    if (!WinHttpSetOption(connection.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(connection.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(connection.request, nullptr))
    {
        error = "Intiface connection failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }

    connection.websocket = WinHttpWebSocketCompleteUpgrade(connection.request, 0);
    if (!connection.websocket)
    {
        error = "websocket complete upgrade failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }

    HINTERNET upgradedRequest = connection.request;
    connection.request = nullptr;
    WinHttpCloseHandle(upgradedRequest);

    if (!SendIntifaceJson(connection, MakeIntifaceServerInfo(connection.nextId++), false))
    {
        error = "Intiface handshake send failed code=" + std::to_string(GetLastError());
        CloseIntifaceConnection(connection);
        return false;
    }

    if (!WaitForIntifaceHandshake(connection, error))
    {
        CloseIntifaceConnection(connection);
        return false;
    }

    StartIntifaceReceiver(connection);

    return true;
}

static bool OpenIntifaceConnection(const Sr6Config& cfg, IntifaceConnection& connection, std::string& error)
{
    const std::vector<std::string> urls = BuildIntifaceUrlCandidates(cfg.intifaceUrl);
    std::string lastError;

    for (size_t i = 0; i < urls.size(); ++i)
    {
        if (OpenIntifaceConnectionUrl(urls[i], connection, lastError))
        {
            if (_stricmp(urls[i].c_str(), cfg.intifaceUrl.c_str()) != 0)
                SetSr6Status("Intiface connected using fallback URL " + urls[i]);
            return true;
        }
    }

    error = lastError.empty() ? "Intiface connection failed" : lastError;
    if (urls.size() > 1)
        error += " | tried " + urls[0] + " and " + urls[1];
    return false;
}

static void DiscoverIntifaceDevices(IntifaceConnection& connection)
{
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("RequestDeviceList", connection.nextId++), false);
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("StartScanning", connection.nextId++), false);
    Sleep(kIntifaceScanDurationMs);
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("StopScanning", connection.nextId++), false);
    Sleep(100);
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("RequestDeviceList", connection.nextId++), false);
    Sleep(kIntifacePostScanReceiveMs);
}

static bool RefreshIntifaceDeviceList(const Sr6Config& cfg, std::string& error)
{
    {
        std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
        g_intifaceDevices.clear();
    }

    IntifaceConnection connection;
    if (!OpenIntifaceConnection(cfg, connection, error))
        return false;

    DiscoverIntifaceDevices(connection);
    SendIntifaceJson(connection, MakeIntifaceStop(connection.nextId++), false);
    CloseIntifaceConnection(connection);
    return true;
}

static bool HasKnownIntifaceCapabilities(const IntifaceDeviceInfo& device)
{
    return
        HasOutputFeature(device.hwPositionWithDuration) ||
        HasOutputFeature(device.position) ||
        HasOutputFeature(device.vibrate) ||
        HasOutputFeature(device.oscillate) ||
        HasOutputFeature(device.constrict) ||
        HasOutputFeature(device.rotate);
}

static bool TryGetIntifaceDeviceInfo(int deviceIndex, IntifaceDeviceInfo& outDevice)
{
    std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
    for (size_t i = 0; i < g_intifaceDevices.size(); ++i)
    {
        if (g_intifaceDevices[i].index == deviceIndex)
        {
            outDevice = g_intifaceDevices[i];
            return true;
        }
    }

    return false;
}

static IntifaceCommandType PickSupportedIntifaceCommandType(int deviceIndex)
{
    IntifaceDeviceInfo device{};
    if (!TryGetIntifaceDeviceInfo(deviceIndex, device) || !HasKnownIntifaceCapabilities(device))
        return IntifaceCommandType::HwPositionWithDuration;

    if (HasOutputFeature(device.hwPositionWithDuration))
        return IntifaceCommandType::HwPositionWithDuration;
    if (HasOutputFeature(device.position))
        return IntifaceCommandType::Position;
    if (HasOutputFeature(device.vibrate))
        return IntifaceCommandType::Vibrate;
    if (HasOutputFeature(device.oscillate))
        return IntifaceCommandType::Oscillate;
    if (HasOutputFeature(device.constrict))
        return IntifaceCommandType::Constrict;
    if (HasOutputFeature(device.rotate))
        return IntifaceCommandType::Rotate;

    return IntifaceCommandType::HwPositionWithDuration;
}

static std::string IntifaceCapabilitiesForDisplay(const IntifaceDeviceInfo& device)
{
    if (!HasKnownIntifaceCapabilities(device))
        return "Unknown signal";

    std::string caps;
    if (HasOutputFeature(device.hwPositionWithDuration))
        caps += caps.empty() ? "HwPosition" : ", HwPosition";
    if (HasOutputFeature(device.position))
        caps += caps.empty() ? "Position" : ", Position";
    if (HasOutputFeature(device.vibrate))
        caps += caps.empty() ? "Vibrate" : ", Vibrate";
    if (HasOutputFeature(device.oscillate))
        caps += caps.empty() ? "Oscillate" : ", Oscillate";
    if (HasOutputFeature(device.constrict))
        caps += caps.empty() ? "Constrict" : ", Constrict";
    if (HasOutputFeature(device.rotate))
        caps += caps.empty() ? "Rotate" : ", Rotate";
    return caps;
}

static std::string GetSr6CommandHistory()
{
    std::lock_guard<std::mutex> lock(g_sr6CommandMutex);
    return g_sr6CommandHistory.empty() ? "(no commands sent yet)\r\n" : g_sr6CommandHistory;
}

static std::string GetSr6LastCommand()
{
    std::lock_guard<std::mutex> lock(g_sr6CommandMutex);
    return g_sr6LastCommand;
}

static HANDLE OpenSerialPort(const Sr6Config& cfg, std::string& error)
{
    if (cfg.portName.empty())
    {
        error = "no COM port selected";
        return INVALID_HANDLE_VALUE;
    }

    const std::string devicePath = SerialDevicePath(cfg.portName);
    HANDLE serial = CreateFileA(
        devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (serial == INVALID_HANDLE_VALUE)
    {
        error = "open failed for " + cfg.portName + " code=" + std::to_string(GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(serial, &dcb))
    {
        error = "GetCommState failed code=" + std::to_string(GetLastError());
        CloseHandle(serial);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = static_cast<DWORD>(cfg.baudRate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(serial, &dcb))
    {
        error = "SetCommState failed code=" + std::to_string(GetLastError());
        CloseHandle(serial);
        return INVALID_HANDLE_VALUE;
    }

    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(serial, &timeouts);

    SetupComm(serial, 4096, 4096);
    PurgeComm(serial, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return serial;
}

static int ToTCode3(float normalized)
{
    // 1.0 is the normalized endpoint and is encoded as the largest legal
    // three-digit TCode magnitude, 999 (which represents 0.999 on the wire).
    const float clamped = ClampFloat(normalized, 0.0f, 1.0f);
    return ClampInt(static_cast<int>(clamped * 999.0f + 0.5f), 0, 999);
}

static int LiveL0Value(float y)
{
    return ToTCode3(1.0f - y);
}

static int LiveR1Value(float x, const Sr6Config& cfg)
{
    float r1Normalized = 1.0f - (ClampFloat(x, -1.0f, 1.0f) + 1.0f) * 0.5f;
    if (cfg.invertX)
        r1Normalized = 1.0f - r1Normalized;

    return ToTCode3(r1Normalized);
}

static std::string FormatSr6AxesCommand(
    float l0,
    float l1,
    float l2,
    float r0,
    float r1,
    float r2,
    int rampMs,
    bool sixAxis)
{
    char command[192]{};
    if (sixAxis)
    {
        std::snprintf(
            command,
            sizeof(command),
            "L0%03dI%03d L1%03dI%03d L2%03dI%03d R0%03dI%03d R1%03dI%03d R2%03dI%03d\r\n",
            ToTCode3(l0), rampMs,
            ToTCode3(l1), rampMs,
            ToTCode3(l2), rampMs,
            ToTCode3(r0), rampMs,
            ToTCode3(r1), rampMs,
            ToTCode3(r2), rampMs);
    }
    else
    {
        std::snprintf(command, sizeof(command), "L0%03dI%03d\r\n", ToTCode3(l0), rampMs);
    }
    return command;
}

static std::string FormatSr6LiveCommand(const MotionInputSnapshot& input, const Sr6Config& cfg)
{
    const int rampMs = ClampInt(cfg.rampMs, 0, cfg.updateMs);
    const float r1 = cfg.invertX ? 1.0f - input.r1 : input.r1;
    return FormatSr6AxesCommand(
        input.l0,
        input.l1,
        input.l2,
        input.r0,
        r1,
        input.r2,
        rampMs,
        cfg.enableSixAxis);
}

static IntifaceDeviceInfo::OutputFeature DefaultIntifaceOutputFeature(IntifaceCommandType type)
{
    IntifaceDeviceInfo::OutputFeature feature{};
    feature.supported = true;
    feature.featureIndex = 0;
    feature.valueMin = 0;
    feature.durationMin = 1;
    feature.durationMax = 5000;

    switch (type)
    {
    case IntifaceCommandType::Vibrate:
    case IntifaceCommandType::Oscillate:
    case IntifaceCommandType::Constrict:
    case IntifaceCommandType::Rotate:
        feature.valueMax = 20;
        feature.durationMax = 1;
        break;
    default:
        feature.valueMax = 100;
        break;
    }

    return feature;
}

static IntifaceDeviceInfo::OutputFeature GetIntifaceOutputFeature(
    const IntifaceDeviceInfo& device,
    IntifaceCommandType type)
{
    switch (type)
    {
    case IntifaceCommandType::Position:
        return HasOutputFeature(device.position) ? device.position : DefaultIntifaceOutputFeature(type);
    case IntifaceCommandType::Vibrate:
        return HasOutputFeature(device.vibrate) ? device.vibrate : DefaultIntifaceOutputFeature(type);
    case IntifaceCommandType::Oscillate:
        return HasOutputFeature(device.oscillate) ? device.oscillate : DefaultIntifaceOutputFeature(type);
    case IntifaceCommandType::Constrict:
        return HasOutputFeature(device.constrict) ? device.constrict : DefaultIntifaceOutputFeature(type);
    case IntifaceCommandType::Rotate:
        return HasOutputFeature(device.rotate) ? device.rotate : DefaultIntifaceOutputFeature(type);
    default:
        return HasOutputFeature(device.hwPositionWithDuration)
            ? device.hwPositionWithDuration
            : DefaultIntifaceOutputFeature(type);
    }
}

static IntifaceDeviceInfo::OutputFeature GetIntifaceOutputFeature(
    int deviceIndex,
    IntifaceCommandType type)
{
    IntifaceDeviceInfo device{};
    if (TryGetIntifaceDeviceInfo(deviceIndex, device))
        return GetIntifaceOutputFeature(device, type);
    return DefaultIntifaceOutputFeature(type);
}

static int ScaleIntifaceRangeValue(double normalized, const IntifaceDeviceInfo::OutputFeature& feature)
{
    normalized = std::max(0.0, std::min(1.0, normalized));
    const int range = std::max(0, feature.valueMax - feature.valueMin);
    return feature.valueMin + static_cast<int>(normalized * range + 0.5);
}

static int ScaleIntifacePositiveValue(double normalized, const IntifaceDeviceInfo::OutputFeature& feature)
{
    normalized = std::max(0.0, std::min(1.0, normalized));
    const int minValue = std::max(0, feature.valueMin);
    const int maxValue = std::max(minValue, feature.valueMax);
    const int range = maxValue - minValue;
    return minValue + static_cast<int>(normalized * range + 0.5);
}

static std::string FormatIntifaceCommand(const Sr6Config& cfg, int l0Value, int r1Value, int durationMs, unsigned int id)
{
    const double position = ClampInt(l0Value, 0, 999) / 999.0;
    const double rotation = ClampInt(r1Value, 0, 999) / 999.0;

    std::ostringstream oss;
    const IntifaceCommandType commandType = PickSupportedIntifaceCommandType(cfg.intifaceDeviceIndex);
    const IntifaceDeviceInfo::OutputFeature feature = GetIntifaceOutputFeature(cfg.intifaceDeviceIndex, commandType);

    oss << "[{\"OutputCmd\":{\"Id\":" << id
        << ",\"DeviceIndex\":" << cfg.intifaceDeviceIndex
        << ",\"FeatureIndex\":" << feature.featureIndex
        << ",\"Command\":{";

    if (commandType == IntifaceCommandType::Position)
    {
        oss << "\"Position\":{\"Value\":" << ScaleIntifaceRangeValue(position, feature) << "}";
    }
    else if (commandType == IntifaceCommandType::Vibrate)
    {
        oss << "\"Vibrate\":{\"Value\":" << ScaleIntifacePositiveValue(position, feature) << "}";
    }
    else if (commandType == IntifaceCommandType::Oscillate)
    {
        oss << "\"Oscillate\":{\"Value\":" << ScaleIntifacePositiveValue(position, feature) << "}";
    }
    else if (commandType == IntifaceCommandType::Constrict)
    {
        oss << "\"Constrict\":{\"Value\":" << ScaleIntifacePositiveValue(position, feature) << "}";
    }
    else if (commandType == IntifaceCommandType::Rotate)
    {
        oss << "\"Rotate\":{\"Value\":" << ScaleIntifacePositiveValue(rotation, feature) << "}";
    }
    else
    {
        const int duration = ClampInt(durationMs, feature.durationMin, feature.durationMax);
        oss << "\"HwPositionWithDuration\":{\"Value\":" << ScaleIntifaceRangeValue(position, feature)
            << ",\"Duration\":" << duration << "}";
    }

    oss << "}}}]";
    return oss.str();
}

static std::string FormatIntifaceLiveCommand(float x, float y, const Sr6Config& cfg, unsigned int id)
{
    return FormatIntifaceCommand(cfg, LiveL0Value(y), LiveR1Value(x, cfg), ClampInt(cfg.rampMs, 1, cfg.updateMs), id);
}

static std::string FormatIntifaceParkCommand(const Sr6Config& cfg, unsigned int id)
{
    return FormatIntifaceCommand(cfg, 999, 500, ClampInt(cfg.rampMs, 1, cfg.updateMs), id);
}

static std::string FormatSr6ParkCommand(const Sr6Config& cfg)
{
    const int rampMs = ClampInt(cfg.rampMs, 0, cfg.updateMs);
    return FormatSr6AxesCommand(
        1.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, rampMs, cfg.enableSixAxis);
}

static int OrgasmRampMs(const Sr6Config& cfg, int intervalMs)
{
    return ClampInt(cfg.rampMs, 0, ClampInt(intervalMs, 1, 1000));
}

static int OrgasmIntervalMs(int hz)
{
    return ClampInt(1000 / ClampInt(hz, 1, kMaxPatternHz), kMinUpdateMs, 1000);
}

static std::string FormatSr6OrgasmCommand(const Sr6Config& cfg, bool high, bool includeRotation, int intervalMs)
{
    const int rampMs = OrgasmRampMs(cfg, intervalMs);
    const int l0 = high ? cfg.orgasmLMax : cfg.orgasmLMin;
    const int r1 = high ? cfg.orgasmRMax : cfg.orgasmRMin;
    const float r1Normalized = includeRotation ? r1 / 999.0f : 0.5f;
    return FormatSr6AxesCommand(
        l0 / 999.0f,
        0.5f,
        0.5f,
        0.5f,
        r1Normalized,
        0.5f,
        rampMs,
        cfg.enableSixAxis);
}

static std::string FormatIntifaceOrgasmCommand(const Sr6Config& cfg, bool high, int intervalMs, unsigned int id)
{
    const int l0 = high ? cfg.orgasmLMax : cfg.orgasmLMin;
    const int r1 = high ? cfg.orgasmRMax : cfg.orgasmRMin;
    return FormatIntifaceCommand(cfg, l0, r1, OrgasmRampMs(cfg, intervalMs), id);
}

static int PushAwayFromCenter500(int value, bool high)
{
    value = ClampInt(value, 0, 999);
    if (value == 500)
        return high ? 600 : 400;

    return ClampInt(value + (value > 500 ? 100 : -100), 0, 999);
}

static std::string FormatSr6AfterCommand(const Sr6Config& cfg, bool high, int intervalMs)
{
    const int rampMs = OrgasmRampMs(cfg, intervalMs);
    const int baseL0 = high ? cfg.orgasmLMax : cfg.orgasmLMin;
    const int l0 = PushAwayFromCenter500(baseL0, high);
    return FormatSr6AxesCommand(
        l0 / 999.0f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, rampMs, cfg.enableSixAxis);
}

static std::string FormatIntifaceAfterCommand(const Sr6Config& cfg, bool high, int intervalMs, unsigned int id)
{
    const int baseL0 = high ? cfg.orgasmLMax : cfg.orgasmLMin;
    const int l0 = PushAwayFromCenter500(baseL0, high);
    return FormatIntifaceCommand(cfg, l0, 500, OrgasmRampMs(cfg, intervalMs), id);
}

static bool SendIntifaceTestPattern(IntifaceConnection& connection, const Sr6Config& cfg)
{
    bool ok = true;
    ok = SendIntifaceJson(connection, FormatIntifaceCommand(cfg, 800, 650, 300, connection.nextId++)) && ok;
    ServiceIntifaceConnection(connection, 20);
    Sleep(350);
    ok = SendIntifaceJson(connection, FormatIntifaceCommand(cfg, 250, 350, 300, connection.nextId++)) && ok;
    ServiceIntifaceConnection(connection, 20);
    Sleep(350);
    return ok;
}

static bool TestIntifaceOutput(const Sr6Config& cfg, std::string& error)
{
    IntifaceConnection connection;
    if (!OpenIntifaceConnection(cfg, connection, error))
        return false;

    DiscoverIntifaceDevices(connection);

    const bool ok = SendIntifaceTestPattern(connection, cfg);
    SendIntifaceJson(connection, MakeIntifaceStop(connection.nextId++), false);
    CloseIntifaceConnection(connection);

    if (!ok)
        error = "Intiface test send failed code=" + std::to_string(GetLastError());
    return ok;
}

static void BeginWorkerIntifaceScan(
    IntifaceConnection& connection,
    bool clearDevices,
    bool guiRequested,
    bool& scanActive,
    unsigned long long& scanStopTick)
{
    if (clearDevices)
    {
        std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
        g_intifaceDevices.clear();
    }

    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("RequestDeviceList", connection.nextId++), false);
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("StartScanning", connection.nextId++), false);
    scanActive = true;
    scanStopTick = GetTickCount64() + kIntifaceScanDurationMs;
    if (guiRequested)
        g_intifaceGuiScanRunning.store(true);
    SetSr6Status(guiRequested ? "Scanning Intiface devices..." : "Intiface connected; scanning devices...");
}

static void FinishWorkerIntifaceScan(
    IntifaceConnection& connection,
    bool guiRequested,
    bool& scanActive,
    unsigned long long& scanStopTick)
{
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("StopScanning", connection.nextId++), false);
    Sleep(100);
    SendIntifaceJson(connection, MakeIntifaceSimpleMessage("RequestDeviceList", connection.nextId++), false);

    scanActive = false;
    scanStopTick = 0;
    if (guiRequested)
        g_intifaceGuiScanRunning.store(false);

    const Sr6Config cfg = GetSr6Config();
    const IntifaceCommandType commandType = PickSupportedIntifaceCommandType(cfg.intifaceDeviceIndex);
    SetSr6Status(
        std::string(guiRequested ? "Intiface devices refreshed" : "Intiface connected") +
        std::string(" device=") + std::to_string(cfg.intifaceDeviceIndex) +
        " cmd=" + IntifaceCommandTypeName(commandType));
    HWND hwnd = g_guiWindow;
    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_INTIFACE_SCAN_DONE, 1, 0);
}

static bool FemaleClimaxActive(const OrgasmSnapshot& orgasm, unsigned long long now)
{
    if (!orgasm.femaleOn || orgasm.femaleTick == 0)
        return false;

    const unsigned long long age = now >= orgasm.femaleTick ? now - orgasm.femaleTick : 0;
    const unsigned long long remainingMs =
        orgasm.femaleOnTime > 0.0f
            ? static_cast<unsigned long long>(orgasm.femaleOnTime * 1000.0f) + 250
            : 1000;
    return age <= remainingMs;
}

static bool FemaleAfterActive(const OrgasmSnapshot& orgasm, unsigned long long now, const Sr6Config& cfg)
{
    if (!orgasm.femaleAfter || orgasm.femaleTick == 0)
        return false;

    const unsigned long long age = now >= orgasm.femaleTick ? now - orgasm.femaleTick : 0;
    const unsigned long long afterFreshMs =
        static_cast<unsigned long long>(OrgasmIntervalMs(cfg.orgasmAfterHz)) + 1000;
    return age <= afterFreshMs;
}

static bool MaleClimaxActive(const OrgasmSnapshot& orgasm, unsigned long long now, const Sr6Config& cfg)
{
    if (!orgasm.maleNow || orgasm.maleStateTick == 0)
        return false;

    const unsigned long long stateAge = now >= orgasm.maleStateTick ? now - orgasm.maleStateTick : 0;
    const unsigned long long freshMs =
        static_cast<unsigned long long>(ClampInt(cfg.updateMs, kMinUpdateMs, kMaxUpdateMs)) + 1000;
    return stateAge <= freshMs;
}

bool Sr6Sync_GetOscillationSuppression(float& deadband)
{
    const Sr6Config cfg = GetSr6Config();
    deadband = ClampFloat(cfg.oscillationDeadband, 0.0f, 0.5f);
    return cfg.activeOscillationSuppression && deadband > 0.0001f;
}

bool Sr6Sync_IsSyntheticClimaxPeakActive()
{
    const Sr6Config cfg = GetSr6Config();
    if (!cfg.orgasmSync)
        return false;
    const OrgasmSnapshot orgasm = OrgasmState_GetSnapshot();
    const unsigned long long now = GetTickCount64();
    return FemaleClimaxActive(orgasm, now) || MaleClimaxActive(orgasm, now, cfg);
}

enum class Sr6OutputMode
{
    Idle,
    Live,
    Park,
    Climax,
    After
};

static std::string PickAutoConnectPort(const Sr6Config& cfg)
{
    if (!cfg.portName.empty())
        return cfg.portName;

    const std::vector<std::string> ports = EnumerateSerialPorts();
    if (ports.size() == 1)
        return ports[0];

    return "";
}

static std::string IntifaceCommandKey(std::string json)
{
    const size_t idKey = json.find("\"Id\":");
    if (idKey == std::string::npos)
        return json;

    const size_t valueStart = idKey + 5;
    size_t valueEnd = valueStart;
    while (valueEnd < json.size() && json[valueEnd] >= '0' && json[valueEnd] <= '9')
        ++valueEnd;

    json.replace(valueStart, valueEnd - valueStart, "0");
    return json;
}

static DWORD WINAPI Sr6WorkerThread(LPVOID)
{
    HANDLE serial = INVALID_HANDLE_VALUE;
    IntifaceConnection intiface;
    bool intifaceConnected = false;
    std::string connectedPort;
    auto nextAutoConnect = std::chrono::steady_clock::now();
    bool lastInserted = false;
    std::string lastCommand;
    Sr6OutputMode lastMode = Sr6OutputMode::Idle;
    bool orgasmHigh = false;
    bool intifaceWorkerScanActive = false;
    bool intifaceWorkerScanGuiRequested = false;
    bool intifaceAutoScanPending = false;
    unsigned long long intifaceAutoScanStartTick = 0;
    unsigned long long intifaceWorkerScanStopTick = 0;

    SetSr6Status("worker started");

    while (g_sr6Running.load())
    {
        Sr6Config cfg = GetSr6Config();
        const bool wantSerial = cfg.outputTarget == OutputTarget::SerialSr6;
        const bool wantIntiface = cfg.outputTarget == OutputTarget::Intiface;

        if ((!cfg.enabled || !wantSerial) && serial != INVALID_HANDLE_VALUE)
            g_sr6DisconnectRequested.store(true);
        if ((!cfg.enabled || !wantIntiface) && intifaceConnected)
            g_sr6DisconnectRequested.store(true);

        if (g_sr6DisconnectRequested.exchange(false))
        {
            g_sr6ConnectionInProgress.store(false);
            g_intifaceWorkerScanRequested.store(false);
            g_intifaceWorkerTestRequested.store(false);
            g_intifaceGuiScanRunning.store(false);
            g_intifaceGuiTestRunning.store(false);
            intifaceWorkerScanActive = false;
            intifaceWorkerScanGuiRequested = false;
            intifaceAutoScanPending = false;
            intifaceAutoScanStartTick = 0;
            intifaceWorkerScanStopTick = 0;
            if (serial != INVALID_HANDLE_VALUE)
            {
                SendSr6Command(serial, "DSTOP\r\n");
                CloseHandle(serial);
                serial = INVALID_HANDLE_VALUE;
                connectedPort.clear();
                SetSr6Status("disconnected");
            }

            if (intifaceConnected)
            {
                SendIntifaceJson(intiface, MakeIntifaceStop(intiface.nextId++), false);
                CloseIntifaceConnection(intiface);
                intifaceConnected = false;
                SetSr6Status("Intiface disconnected");
            }

            g_sr6Connected.store(false);
        }

        if (serial == INVALID_HANDLE_VALUE && !intifaceConnected && cfg.enabled)
        {
            bool shouldConnect = g_sr6ConnectRequested.exchange(false);
            if (!shouldConnect && cfg.autoConnect && std::chrono::steady_clock::now() >= nextAutoConnect)
            {
                shouldConnect = true;
                nextAutoConnect = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            }

            if (shouldConnect)
            {
                g_sr6ConnectionInProgress.store(true);
                SetSr6Status(wantIntiface ? "connecting Intiface..." : "connecting serial...");
                std::string finalStatus;
                if (wantIntiface)
                {
                    std::string error;
                    if (OpenIntifaceConnection(cfg, intiface, error))
                    {
                        intifaceConnected = true;
                        g_sr6Connected.store(true);
                        const IntifaceCommandType commandType = PickSupportedIntifaceCommandType(cfg.intifaceDeviceIndex);
                        finalStatus = "Intiface connected " + cfg.intifaceUrl + " device=" + std::to_string(cfg.intifaceDeviceIndex) + " cmd=" + IntifaceCommandTypeName(commandType);
                        intifaceAutoScanPending = true;
                        intifaceAutoScanStartTick = GetTickCount64() + 500;
                    }
                    else
                    {
                        finalStatus = error;
                    }
                }
                else
                {
                    Sr6Config connectCfg = cfg;
                    connectCfg.portName = PickAutoConnectPort(cfg);

                    if (connectCfg.portName.empty())
                    {
                        finalStatus = "waiting for COM port";
                    }
                    else
                    {
                        std::string error;
                        serial = OpenSerialPort(connectCfg, error);
                        if (serial == INVALID_HANDLE_VALUE)
                        {
                            finalStatus = error;
                        }
                        else
                        {
                            connectedPort = connectCfg.portName;
                            g_sr6Connected.store(true);
                            finalStatus = "connected " + connectedPort + " @" + std::to_string(connectCfg.baudRate);
                            SendSr6Command(serial, "D0\r\n");
                        }
                    }
                }
                g_sr6ConnectionInProgress.store(false);
                SetSr6Status(finalStatus.empty() ? GetSr6Status() : finalStatus);
            }
        }

        if (serial != INVALID_HANDLE_VALUE || intifaceConnected)
        {
            cfg = GetSr6Config();
            const bool outputIntiface = intifaceConnected && cfg.outputTarget == OutputTarget::Intiface;

            if (outputIntiface &&
                intifaceAutoScanPending &&
                !intifaceWorkerScanActive &&
                GetTickCount64() >= intifaceAutoScanStartTick)
            {
                intifaceAutoScanPending = false;
                intifaceAutoScanStartTick = 0;
                BeginWorkerIntifaceScan(intiface, true, false, intifaceWorkerScanActive, intifaceWorkerScanStopTick);
                intifaceWorkerScanGuiRequested = false;
            }

            if (outputIntiface && g_intifaceWorkerScanRequested.exchange(false))
            {
                if (intifaceWorkerScanActive)
                {
                    intifaceWorkerScanGuiRequested = true;
                    g_intifaceGuiScanRunning.store(true);
                    SetSr6Status("Scanning Intiface devices...");
                }
                else
                {
                    BeginWorkerIntifaceScan(intiface, true, true, intifaceWorkerScanActive, intifaceWorkerScanStopTick);
                    intifaceWorkerScanGuiRequested = true;
                }
            }

            if (outputIntiface && intifaceWorkerScanActive)
            {
                ServiceIntifaceConnection(intiface, 20);
                if (GetTickCount64() >= intifaceWorkerScanStopTick)
                {
                    const bool guiRequested = intifaceWorkerScanGuiRequested;
                    FinishWorkerIntifaceScan(intiface, guiRequested, intifaceWorkerScanActive, intifaceWorkerScanStopTick);
                    intifaceWorkerScanGuiRequested = false;
                }
            }

            if (outputIntiface && g_intifaceWorkerTestRequested.exchange(false))
            {
                const bool ok = SendIntifaceTestPattern(intiface, cfg);
                g_intifaceGuiTestRunning.store(false);
                SetSr6Status(ok ? "Intiface test sent" : "Intiface test send failed code=" + std::to_string(GetLastError()));
                HWND hwnd = g_guiWindow;
                if (hwnd && IsWindow(hwnd))
                    PostMessageA(hwnd, WM_SR6_INTIFACE_TEST_DONE, ok ? 1 : 0, 0);
            }

            // The curve sampler predicts exactly one output interval ahead.
            // With a 100 ms send interval and I090, for example, this sends
            // the animation's t+100 ms point and reaches it at about t+90 ms.
            MotionInput_SetLookaheadMs(static_cast<unsigned int>(cfg.updateMs));
            const MotionInputSnapshot input = MotionInput_GetSelected();
            const bool inserted = input.inserted;
            const float x = input.x;
            const float y = input.y;
            const OrgasmSnapshot orgasm = OrgasmState_GetSnapshot();
            const unsigned long long now = GetTickCount64();
            const bool femaleClimax = FemaleClimaxActive(orgasm, now);
            const bool maleClimax = MaleClimaxActive(orgasm, now, cfg);
            const bool climaxActive =
                cfg.orgasmSync &&
                (femaleClimax || maleClimax);
            const bool dualClimax = femaleClimax && maleClimax;
            const bool afterActive =
                cfg.orgasmSync &&
                !climaxActive &&
                !inserted &&
                FemaleAfterActive(orgasm, now, cfg);

            std::string command;
            int sleepMs = ClampInt(cfg.updateMs, kMinUpdateMs, kMaxUpdateMs);
            Sr6OutputMode mode = Sr6OutputMode::Idle;

            if (climaxActive)
            {
                mode = Sr6OutputMode::Climax;
                const int climaxHz = ClampInt(
                    dualClimax ? cfg.orgasmClimaxHz * 2 : cfg.orgasmClimaxHz,
                    1,
                    kMaxPatternHz);
                const int intervalMs = OrgasmIntervalMs(climaxHz);
                orgasmHigh = !orgasmHigh;
                command = outputIntiface
                    ? FormatIntifaceOrgasmCommand(cfg, orgasmHigh, intervalMs, intiface.nextId++)
                    : FormatSr6OrgasmCommand(cfg, orgasmHigh, true, intervalMs);
                sleepMs = intervalMs;
            }
            else if (afterActive)
            {
                mode = Sr6OutputMode::After;
                const int intervalMs = OrgasmIntervalMs(cfg.orgasmAfterHz);
                orgasmHigh = !orgasmHigh;
                command = outputIntiface
                    ? FormatIntifaceAfterCommand(cfg, orgasmHigh, intervalMs, intiface.nextId++)
                    : FormatSr6AfterCommand(cfg, orgasmHigh, intervalMs);
                sleepMs = intervalMs;
            }
            else if (inserted)
            {
                mode = Sr6OutputMode::Live;
                command = outputIntiface
                    ? FormatIntifaceLiveCommand(x, y, cfg, intiface.nextId++)
                    : FormatSr6LiveCommand(input, cfg);
            }
            else if (cfg.parkOnNotInserted && lastInserted)
            {
                mode = Sr6OutputMode::Park;
                command = outputIntiface
                    ? FormatIntifaceParkCommand(cfg, intiface.nextId++)
                    : FormatSr6ParkCommand(cfg);
            }

            if (mode != lastMode)
            {
                lastCommand.clear();
                if (mode != Sr6OutputMode::Climax && mode != Sr6OutputMode::After)
                    orgasmHigh = false;
                lastMode = mode;
            }

            const std::string commandKey = outputIntiface ? IntifaceCommandKey(command) : command;
            if (!command.empty() && commandKey != lastCommand)
            {
                const bool sent = outputIntiface
                    ? SendIntifaceJson(intiface, command)
                    : SendSr6Command(serial, command);
                if (!sent)
                {
                    SetSr6Status(std::string(outputIntiface ? "Intiface write failed" : "write failed on " + connectedPort) + " code=" + std::to_string(GetLastError()));
                    if (outputIntiface)
                    {
                        CloseIntifaceConnection(intiface);
                        intifaceConnected = false;
                    }
                    else
                    {
                        CloseHandle(serial);
                        serial = INVALID_HANDLE_VALUE;
                        connectedPort.clear();
                    }
                    g_sr6Connected.store(false);
                    lastCommand.clear();
                }
                else
                {
                    lastCommand = commandKey;
                }
            }

            if (outputIntiface)
                ServiceIntifaceConnection(intiface, 1);

            lastInserted = inserted;
            Sleep(static_cast<DWORD>(sleepMs));
        }
        else
        {
            if (g_intifaceWorkerScanRequested.exchange(false))
            {
                g_intifaceGuiScanRunning.store(false);
                SetSr6Status("Intiface scan needs an active connection");
            }
            if (g_intifaceWorkerTestRequested.exchange(false))
            {
                g_intifaceGuiTestRunning.store(false);
                SetSr6Status("Intiface test needs an active connection");
            }
            lastInserted = false;
            lastCommand.clear();
            lastMode = Sr6OutputMode::Idle;
            orgasmHigh = false;
            Sleep(100);
        }
    }

    if (serial != INVALID_HANDLE_VALUE)
    {
        SendSr6Command(serial, "DSTOP\r\n");
        CloseHandle(serial);
    }
    if (intifaceConnected)
    {
        SendIntifaceJson(intiface, MakeIntifaceStop(intiface.nextId++), false);
        CloseIntifaceConnection(intiface);
    }
    g_sr6Connected.store(false);
    g_sr6ConnectionInProgress.store(false);

    SetSr6Status("worker stopped");
    return 0;
}

enum Sr6GuiControlId
{
    IDC_SR6_STATUS = 2001,
    IDC_SR6_PORT,
    IDC_SR6_REFRESH,
    IDC_SR6_CONNECT,
    IDC_SR6_DISCONNECT,
    IDC_SR6_ENABLED,
    IDC_SR6_AUTO,
    IDC_SR6_INVERT_X,
    IDC_SR6_PARK,
    IDC_SR6_R1,
    IDC_SR6_UPDATE_SLIDER,
    IDC_SR6_RAMP_SLIDER,
    IDC_SR6_UPDATE_LABEL,
    IDC_SR6_RAMP_LABEL,
    IDC_SR6_CONNECTED_LABEL,
    IDC_SR6_INSERTED_LABEL,
    IDC_SR6_XY_LABEL,
    IDC_SR6_LAST_COMMAND_LABEL,
    IDC_SR6_COMMAND_EDIT,
    IDC_SR6_OUTPUT_TARGET,
    IDC_SR6_INTIFACE_URL,
    IDC_SR6_INTIFACE_DEVICE,
    IDC_SR6_INTIFACE_SCAN,
    IDC_SR6_INTIFACE_TEST,
    IDC_SR6_LANGUAGE,
    IDC_SR6_GUI_HOTKEY,
    IDC_SR6_CONSOLE_HOTKEY,
    IDC_SR6_ORGASM_ENABLED,
    IDC_SR6_ORGASM_STATE_LABEL,
    IDC_SR6_ORGASM_CLIMAX_HZ,
    IDC_SR6_ORGASM_AFTER_HZ,
    IDC_SR6_ORGASM_L_RANGE,
    IDC_SR6_ORGASM_R_RANGE,
    IDC_SR6_ORGASM_L_RANGE_LABEL,
    IDC_SR6_ORGASM_R_RANGE_LABEL,
    IDC_SR6_LABEL_OUTPUT,
    IDC_SR6_LABEL_INTIFACE,
    IDC_SR6_LABEL_DEVICE,
    IDC_SR6_LABEL_LANGUAGE,
    IDC_SR6_LABEL_GUI_KEY,
    IDC_SR6_LABEL_CONSOLE_KEY,
    IDC_SR6_LABEL_CLIMAX_HZ,
    IDC_SR6_LABEL_AFTER_HZ,
    IDC_SR6_COMMAND_GROUP,
    IDC_SR6_INTIFACE_GROUP,
    IDC_SR6_OSCILLATION_ENABLED,
    IDC_SR6_OSCILLATION_LABEL,
    IDC_SR6_OSCILLATION_SLIDER,
    IDC_SR6_OSCILLATION_STATE
};

static void SetCheck(HWND hwnd, int id, bool checked)
{
    SendMessageA(GetDlgItem(hwnd, id), BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

static bool GetCheck(HWND hwnd, int id)
{
    return SendMessageA(GetDlgItem(hwnd, id), BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void SetTrackbarRange(HWND control, int minValue, int maxValue)
{
    SendMessageA(control, TBM_SETRANGE, TRUE, MAKELPARAM(minValue, maxValue));
}

static int ReadIntEdit(HWND hwnd, int id, int fallback, int minValue, int maxValue)
{
    BOOL translated = FALSE;
    const int value = GetDlgItemInt(hwnd, id, &translated, FALSE);
    if (!translated)
        return fallback;
    return ClampInt(value, minValue, maxValue);
}

constexpr UINT WM_RANGE_SLIDER_CHANGED = WM_APP + 120;
constexpr const char* kRangeSliderClassName = "MocaLoveReliveRangeSlider";

struct RangeSliderState
{
    int minValue = 0;
    int maxValue = 999;
    int low = 400;
    int high = 600;
    int activeThumb = 0;
    bool dragging = false;
};

static int RangeSliderTrackLeft(const RECT& rc)
{
    return rc.left + 12;
}

static int RangeSliderTrackRight(const RECT& rc)
{
    return rc.right - 12;
}

static int RangeSliderValueToX(const RangeSliderState* state, const RECT& rc, int value)
{
    const int left = RangeSliderTrackLeft(rc);
    const int right = RangeSliderTrackRight(rc);
    const int width = std::max(1, right - left);
    const int clamped = ClampInt(value, state->minValue, state->maxValue);
    return left + (clamped - state->minValue) * width / std::max(1, state->maxValue - state->minValue);
}

static int RangeSliderXToValue(const RangeSliderState* state, const RECT& rc, int x)
{
    const int left = RangeSliderTrackLeft(rc);
    const int right = RangeSliderTrackRight(rc);
    const int clampedX = ClampInt(x, left, right);
    const int width = std::max(1, right - left);
    return state->minValue + (clampedX - left) * (state->maxValue - state->minValue) / width;
}

static RangeSliderState* GetRangeSliderState(HWND hwnd)
{
    return reinterpret_cast<RangeSliderState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
}

static void NotifyRangeSliderChanged(HWND hwnd, bool finalChange)
{
    HWND parent = GetParent(hwnd);
    if (parent)
        PostMessageA(parent, WM_RANGE_SLIDER_CHANGED, static_cast<WPARAM>(GetDlgCtrlID(hwnd)), finalChange ? 1 : 0);
}

static LRESULT CALLBACK RangeSliderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    RangeSliderState* state = GetRangeSliderState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new RangeSliderState()));
        return 0;
    case WM_DESTROY:
        delete state;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_LBUTTONDOWN:
    {
        if (!state)
            return 0;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int x = GET_X_LPARAM(lParam);
        const int lowX = RangeSliderValueToX(state, rc, state->low);
        const int highX = RangeSliderValueToX(state, rc, state->high);
        state->activeThumb = std::abs(x - lowX) <= std::abs(x - highX) ? 1 : 2;
        state->dragging = true;
        SetCapture(hwnd);
        SendMessageA(hwnd, WM_MOUSEMOVE, wParam, lParam);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!state || !state->dragging)
            return 0;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int value = RangeSliderXToValue(state, rc, GET_X_LPARAM(lParam));
        if (state->activeThumb == 1)
            state->low = ClampInt(value, state->minValue, state->high);
        else
            state->high = ClampInt(value, state->low, state->maxValue);
        InvalidateRect(hwnd, nullptr, TRUE);
        NotifyRangeSliderChanged(hwnd, false);
        return 0;
    }
    case WM_LBUTTONUP:
        if (state)
        {
            state->dragging = false;
            state->activeThumb = 0;
        }
        ReleaseCapture();
        NotifyRangeSliderChanged(hwnd, true);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(kGuiBgColor);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        if (state)
        {
            const int cy = (rc.top + rc.bottom) / 2;
            const int left = RangeSliderTrackLeft(rc);
            const int right = RangeSliderTrackRight(rc);
            const int lowX = RangeSliderValueToX(state, rc, state->low);
            const int highX = RangeSliderValueToX(state, rc, state->high);

            HPEN basePen = CreatePen(PS_SOLID, 4, kGuiBorderColor);
            HPEN activePen = CreatePen(PS_SOLID, 5, kGuiAccentColor);
            HPEN oldPen = static_cast<HPEN>(SelectObject(dc, basePen));
            MoveToEx(dc, left, cy, nullptr);
            LineTo(dc, right, cy);
            SelectObject(dc, activePen);
            MoveToEx(dc, lowX, cy, nullptr);
            LineTo(dc, highX, cy);
            SelectObject(dc, oldPen);
            DeleteObject(basePen);
            DeleteObject(activePen);

            HBRUSH thumbBrush = CreateSolidBrush(kGuiEditBgColor);
            HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, thumbBrush));
            HPEN thumbPen = CreatePen(PS_SOLID, 1, kGuiMutedTextColor);
            oldPen = static_cast<HPEN>(SelectObject(dc, thumbPen));
            Rectangle(dc, lowX - 6, cy - 10, lowX + 6, cy + 11);
            Rectangle(dc, highX - 6, cy - 10, highX + 6, cy + 11);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(thumbBrush);
            DeleteObject(thumbPen);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void SetRangeSliderValues(HWND hwnd, int low, int high)
{
    RangeSliderState* state = GetRangeSliderState(hwnd);
    if (!state)
        return;
    state->low = ClampInt(low, state->minValue, state->maxValue);
    state->high = ClampInt(high, state->minValue, state->maxValue);
    if (state->low > state->high)
        std::swap(state->low, state->high);
    InvalidateRect(hwnd, nullptr, TRUE);
}

static void GetRangeSliderValues(HWND hwnd, int& low, int& high)
{
    RangeSliderState* state = GetRangeSliderState(hwnd);
    if (!state)
        return;
    low = state->low;
    high = state->high;
}

struct HotkeyOption
{
    int vk;
    const char* name;
};

static const std::vector<HotkeyOption>& GetHotkeyOptions()
{
    static const std::vector<HotkeyOption> options = []()
    {
        std::vector<HotkeyOption> values;
        values.push_back({ 0, "None" });
        for (int vk = 'A'; vk <= 'Z'; ++vk)
        {
            static char names[26][2]{};
            names[vk - 'A'][0] = static_cast<char>(vk);
            names[vk - 'A'][1] = '\0';
            values.push_back({ vk, names[vk - 'A'] });
        }
        values.push_back({ VK_F1, "F1" });
        values.push_back({ VK_F2, "F2" });
        values.push_back({ VK_F3, "F3" });
        values.push_back({ VK_F4, "F4" });
        values.push_back({ VK_F5, "F5" });
        values.push_back({ VK_F6, "F6" });
        values.push_back({ VK_F7, "F7" });
        values.push_back({ VK_F8, "F8" });
        values.push_back({ VK_F9, "F9" });
        values.push_back({ VK_F10, "F10" });
        values.push_back({ VK_F11, "F11" });
        values.push_back({ VK_F12, "F12" });
        return values;
    }();
    return options;
}

static const char* HotkeyName(int vk)
{
    const std::vector<HotkeyOption>& options = GetHotkeyOptions();
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].vk == vk)
            return options[i].name;
    }
    return "Unknown";
}

static bool IsKnownHotkey(int vk)
{
    const std::vector<HotkeyOption>& options = GetHotkeyOptions();
    for (size_t i = 0; i < options.size(); ++i)
    {
        if (options[i].vk == vk)
            return true;
    }
    return false;
}

static void PopulateHotkeyCombo(HWND combo, int selectedVk)
{
    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    const std::vector<HotkeyOption>& options = GetHotkeyOptions();
    int selectedIndex = 0;
    for (size_t i = 0; i < options.size(); ++i)
    {
        const int index = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(options[i].name)));
        SendMessageA(combo, CB_SETITEMDATA, index, static_cast<LPARAM>(options[i].vk));
        if (options[i].vk == selectedVk)
            selectedIndex = index;
    }
    SendMessageA(combo, CB_SETCURSEL, selectedIndex, 0);
}

static const wchar_t* UiText(const Sr6Config& cfg, const wchar_t* english, const wchar_t* chinese)
{
    return IsChinese(cfg) ? chinese : english;
}

static void SetControlText(HWND hwnd, int id, const wchar_t* english, const wchar_t* chinese)
{
    Sr6Config cfg = GetSr6Config();
    HWND control = GetDlgItem(hwnd, id);
    if (control)
        SetWindowTextW(control, UiText(cfg, english, chinese));
}

static void PopulateLanguageCombo(HWND combo, UiLanguage selected)
{
    if (!combo)
        return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    const int englishIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English")));
    SendMessageW(combo, CB_SETITEMDATA, englishIndex, static_cast<LPARAM>(static_cast<int>(UiLanguage::English)));
    const int chineseIndex = static_cast<int>(SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\u4e2d\u6587")));
    SendMessageW(combo, CB_SETITEMDATA, chineseIndex, static_cast<LPARAM>(static_cast<int>(UiLanguage::Chinese)));
    SendMessageW(combo, CB_SETCURSEL, selected == UiLanguage::Chinese ? chineseIndex : englishIndex, 0);
}

static UiLanguage ReadLanguageCombo(HWND combo, UiLanguage fallback)
{
    if (!combo)
        return fallback;

    const int selectedIndex = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR)
        return fallback;

    const LRESULT data = SendMessageW(combo, CB_GETITEMDATA, selectedIndex, 0);
    if (data == CB_ERR)
        return fallback;

    return ClampUiLanguage(static_cast<int>(data));
}

static void PopulateOutputTargetCombo(HWND combo, OutputTarget selected)
{
    if (!combo)
        return;

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);
    const int serialIndex = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Serial SR6")));
    SendMessageA(combo, CB_SETITEMDATA, serialIndex, static_cast<LPARAM>(static_cast<int>(OutputTarget::SerialSr6)));
    const int intifaceIndex = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Intiface Central")));
    SendMessageA(combo, CB_SETITEMDATA, intifaceIndex, static_cast<LPARAM>(static_cast<int>(OutputTarget::Intiface)));
    SendMessageA(combo, CB_SETCURSEL, selected == OutputTarget::Intiface ? intifaceIndex : serialIndex, 0);
}

static OutputTarget ReadOutputTargetCombo(HWND combo, OutputTarget fallback)
{
    if (!combo)
        return fallback;

    const int selectedIndex = static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR)
        return fallback;

    const LRESULT data = SendMessageA(combo, CB_GETITEMDATA, selectedIndex, 0);
    if (data == CB_ERR)
        return fallback;

    return ClampOutputTarget(static_cast<int>(data));
}

static void PopulateIntifaceDeviceCombo(HWND combo, int selectedDevice)
{
    if (!combo)
        return;

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);

    std::vector<IntifaceDeviceInfo> devices;
    {
        std::lock_guard<std::mutex> lock(g_intifaceDeviceMutex);
        devices = g_intifaceDevices;
    }

    if (devices.empty())
        devices.push_back({ selectedDevice, "Device " + std::to_string(selectedDevice) + " (not scanned)" });

    int selectedIndex = 0;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        std::string label = std::to_string(devices[i].index) + ": " + devices[i].name;
        const std::string caps = IntifaceCapabilitiesForDisplay(devices[i]);
        label += " [" + caps + "]";
        const int index = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
        SendMessageA(combo, CB_SETITEMDATA, index, static_cast<LPARAM>(devices[i].index));
        if (devices[i].index == selectedDevice)
            selectedIndex = index;
    }

    SendMessageA(combo, CB_SETCURSEL, selectedIndex, 0);
}

static int ReadIntifaceDeviceCombo(HWND combo, int fallback)
{
    if (!combo)
        return fallback;

    const int selectedIndex = static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR)
        return fallback;

    const LRESULT data = SendMessageA(combo, CB_GETITEMDATA, selectedIndex, 0);
    if (data == CB_ERR)
        return fallback;

    return ClampInt(static_cast<int>(data), 0, 99);
}

static int ReadHotkeyCombo(HWND combo, int fallbackVk)
{
    const int selectedIndex = static_cast<int>(SendMessageA(combo, CB_GETCURSEL, 0, 0));
    if (selectedIndex == CB_ERR)
        return fallbackVk;

    const LRESULT data = SendMessageA(combo, CB_GETITEMDATA, selectedIndex, 0);
    if (data == CB_ERR)
        return fallbackVk;

    return static_cast<int>(data);
}

static void ApplyLanguageToGui(HWND hwnd)
{
    Sr6Config cfg = GetSr6Config();
    SetWindowTextW(hwnd, UiText(cfg, L"Moka Love Relive Device Sync", L"Moka Love Relive \u8bbe\u5907\u540c\u6b65"));
    PopulateLanguageCombo(GetDlgItem(hwnd, IDC_SR6_LANGUAGE), cfg.language);

    SetControlText(hwnd, IDC_SR6_LABEL_OUTPUT, L"Output", L"\u8f93\u51fa");
    SetControlText(hwnd, IDC_SR6_REFRESH, L"Refresh", L"\u5237\u65b0");
    SetControlText(hwnd, IDC_SR6_CONNECT, L"Connect", L"\u8fde\u63a5");
    SetControlText(hwnd, IDC_SR6_DISCONNECT, L"Disconnect", L"\u65ad\u5f00");
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_INTIFACE_GROUP), UiText(cfg, L"Intiface Central Settings", L"Intiface Central 设置"));
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_INTIFACE_GROUP), UiText(cfg, L"Intiface Central Settings", L"Intiface Central \u8bbe\u7f6e"));
    SetControlText(hwnd, IDC_SR6_LABEL_INTIFACE, L"Intiface", L"Intiface");
    SetControlText(hwnd, IDC_SR6_INTIFACE_SCAN, L"Scan IC", L"\u626b\u63cf IC");
    SetControlText(hwnd, IDC_SR6_INTIFACE_TEST, L"Test IC", L"\u6d4b\u8bd5 IC");
    SetControlText(hwnd, IDC_SR6_LABEL_DEVICE, L"Device", L"\u8bbe\u5907");
    SetControlText(hwnd, IDC_SR6_LABEL_LANGUAGE, L"Language", L"\u8bed\u8a00");
    SetControlText(hwnd, IDC_SR6_ENABLED, L"Enable sync", L"\u542f\u7528\u540c\u6b65");
    SetControlText(hwnd, IDC_SR6_AUTO, L"Auto connect", L"\u81ea\u52a8\u8fde\u63a5");
    SetControlText(hwnd, IDC_SR6_R1, L"Enable SR6 6-axis", L"\u542f\u7528 SR6 \u516d\u8f74");
    SetControlText(hwnd, IDC_SR6_INVERT_X, L"Invert X", L"\u53cd\u8f6c X");
    SetControlText(hwnd, IDC_SR6_PARK, L"Park on notInsert", L"\u672a\u63d2\u5165\u65f6\u5f52\u4f4d");
    SetControlText(hwnd, IDC_SR6_LABEL_GUI_KEY, L"GUI key", L"GUI \u70ed\u952e");
    SetControlText(hwnd, IDC_SR6_LABEL_CONSOLE_KEY, L"Console key", L"\u63a7\u5236\u53f0\u70ed\u952e");
    SetControlText(hwnd, IDC_SR6_ORGASM_ENABLED, L"Enable orgasm sync", L"\u542f\u7528\u9ad8\u6f6e\u540c\u6b65");
    SetControlText(hwnd, IDC_SR6_LABEL_CLIMAX_HZ, L"Climax Hz", L"\u9ad8\u6f6e Hz");
    SetControlText(hwnd, IDC_SR6_LABEL_AFTER_HZ, L"After Hz", L"\u4f59\u97f5 Hz");
    SetControlText(
        hwnd,
        IDC_SR6_OSCILLATION_ENABLED,
        L"Active L0 oscillation suppression",
        L"\u4e3b\u52a8\u6291\u5236 L0 \u9707\u8361");
    SetControlText(hwnd, IDC_SR6_COMMAND_GROUP, L"Commands sent to selected output", L"\u53d1\u9001\u5230\u5f53\u524d\u8f93\u51fa\u7684\u6307\u4ee4");

    InvalidateRect(hwnd, nullptr, TRUE);
}

static void RefreshCommandDisplay(HWND hwnd, bool force = false)
{
    static std::string lastHistory;
    HWND commandEdit = GetDlgItem(hwnd, IDC_SR6_COMMAND_EDIT);
    if (!commandEdit)
        return;

    const std::string history = GetSr6CommandHistory();
    if (!force && history == lastHistory)
        return;

    lastHistory = history;
    SetWindowTextA(commandEdit, history.c_str());
    SendMessageA(commandEdit, EM_SETSEL, static_cast<WPARAM>(history.size()), static_cast<LPARAM>(history.size()));
    SendMessageA(commandEdit, EM_SCROLLCARET, 0, 0);
}

static void RedrawControlNow(HWND hwnd, int id)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
        RedrawWindow(control, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

static void RedrawDeviceParamArea(HWND hwnd)
{
    const int ids[] = {
        IDC_SR6_ENABLED,
        IDC_SR6_AUTO,
        IDC_SR6_OUTPUT_TARGET,
        IDC_SR6_PORT,
        IDC_SR6_REFRESH,
        IDC_SR6_CONNECT,
        IDC_SR6_DISCONNECT,
        IDC_SR6_INTIFACE_GROUP,
        IDC_SR6_INTIFACE_URL,
        IDC_SR6_INTIFACE_DEVICE,
        IDC_SR6_INTIFACE_SCAN,
        IDC_SR6_INTIFACE_TEST,
        IDC_SR6_R1,
        IDC_SR6_INVERT_X,
        IDC_SR6_PARK,
        IDC_SR6_UPDATE_LABEL,
        IDC_SR6_UPDATE_SLIDER,
        IDC_SR6_RAMP_LABEL,
        IDC_SR6_RAMP_SLIDER,
        IDC_SR6_OSCILLATION_ENABLED,
        IDC_SR6_OSCILLATION_LABEL,
        IDC_SR6_OSCILLATION_SLIDER,
        IDC_SR6_OSCILLATION_STATE,
        IDC_SR6_ORGASM_ENABLED,
        IDC_SR6_ORGASM_CLIMAX_HZ,
        IDC_SR6_ORGASM_AFTER_HZ,
        IDC_SR6_ORGASM_L_RANGE_LABEL,
        IDC_SR6_ORGASM_L_RANGE,
        IDC_SR6_ORGASM_R_RANGE_LABEL,
        IDC_SR6_ORGASM_R_RANGE
    };

    for (int id : ids)
        RedrawControlNow(hwnd, id);
}

static void SetControlEnabled(HWND hwnd, int id, bool enabled)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
        EnableWindow(control, enabled ? TRUE : FALSE);
}

static void SetControlVisible(HWND hwnd, int id, bool visible)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
        ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
}

static void UpdateOutputTargetControlState(HWND hwnd, const Sr6Config& cfg)
{
    const bool isSerial = cfg.outputTarget == OutputTarget::SerialSr6;
    const bool isIntiface = cfg.outputTarget == OutputTarget::Intiface;
    const bool enabled = cfg.enabled;
    const bool connected = g_sr6Connected.load();
    const bool connecting = g_sr6ConnectionInProgress.load();
    const bool intifaceBusy = g_intifaceGuiScanRunning.load() || g_intifaceGuiTestRunning.load();
    const bool idleConfigurable = enabled && !connected && !connecting && !intifaceBusy;
    const bool intifaceActionReady = enabled && isIntiface && !connecting && !intifaceBusy;

    SetControlVisible(hwnd, IDC_SR6_PORT, isSerial);
    SetControlVisible(hwnd, IDC_SR6_REFRESH, isSerial);
    SetControlVisible(hwnd, IDC_SR6_R1, isSerial);
    SetControlVisible(hwnd, IDC_SR6_INTIFACE_GROUP, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_LABEL_INTIFACE, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_INTIFACE_URL, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_LABEL_DEVICE, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_INTIFACE_DEVICE, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_INTIFACE_SCAN, isIntiface);
    SetControlVisible(hwnd, IDC_SR6_INTIFACE_TEST, isIntiface);

    SetControlEnabled(hwnd, IDC_SR6_PORT, idleConfigurable && isSerial);
    SetControlEnabled(hwnd, IDC_SR6_REFRESH, idleConfigurable && isSerial);
    SetControlEnabled(hwnd, IDC_SR6_INTIFACE_URL, idleConfigurable && isIntiface);
    SetControlEnabled(hwnd, IDC_SR6_INTIFACE_DEVICE, intifaceActionReady);
    SetControlEnabled(hwnd, IDC_SR6_INTIFACE_SCAN, intifaceActionReady);
    SetControlEnabled(hwnd, IDC_SR6_INTIFACE_TEST, intifaceActionReady);
    SetControlEnabled(hwnd, IDC_SR6_CONNECT, idleConfigurable);
    SetControlEnabled(hwnd, IDC_SR6_DISCONNECT, connected);
}

static void UpdateSr6GuiLabels(HWND hwnd, bool forceParamRedraw = false)
{
    Sr6Config cfg = GetSr6Config();
    cfg.updateMs = ClampInt(cfg.updateMs, kMinUpdateMs, kMaxUpdateMs);
    cfg.rampMs = NormalizeRampMs(cfg.rampMs, cfg.updateMs);

    wchar_t label[512]{};
    swprintf_s(
        label,
        UiText(
            cfg,
            L"Device send interval: %d ms (%.1f Hz)",
            L"\u8bbe\u5907\u53d1\u9001\u95f4\u9694: %d \u6beb\u79d2 (%.1f Hz)"),
        cfg.updateMs,
        1000.0 / cfg.updateMs);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_UPDATE_LABEL), label);

    swprintf_s(
        label,
        cfg.outputTarget == OutputTarget::Intiface
            ? UiText(cfg, L"Command duration: %d ms", L"\u6307\u4ee4\u6301\u7eed\u65f6\u95f4: %d \u6beb\u79d2")
            : UiText(cfg, L"TCode I duration: %d ms", L"TCode I \u6301\u7eed\u65f6\u95f4: %d \u6beb\u79d2"),
        cfg.rampMs);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_RAMP_LABEL), label);

    swprintf_s(
        label,
        UiText(
            cfg,
            L"Oscillation deadband: %.0f%% (0 keeps authored curve)",
            L"\u9707\u8361\u6291\u5236\u9608\u503c: %.0f%%\uff080 \u4fdd\u7559\u539f\u59cb\u66f2\u7ebf\uff09"),
        cfg.oscillationDeadband * 100.0f);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_OSCILLATION_LABEL), label);

    swprintf_s(label, UiText(cfg, L"L range: %03d - %03d", L"L \u884c\u7a0b: %03d - %03d"), cfg.orgasmLMin, cfg.orgasmLMax);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_ORGASM_L_RANGE_LABEL), label);

    swprintf_s(label, UiText(cfg, L"R range: %03d - %03d", L"R \u503e\u659c: %03d - %03d"), cfg.orgasmRMin, cfg.orgasmRMax);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_ORGASM_R_RANGE_LABEL), label);

    const std::wstring status = Utf8ToWide(GetSr6Status());
    const std::wstring outputName = Utf8ToWide(OutputTargetName(cfg.outputTarget));
    swprintf_s(label, UiText(cfg, L"Status[%s]: %s", L"\u72b6\u6001[%s]: %s"), outputName.c_str(), status.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_STATUS), label);

    swprintf_s(label, UiText(cfg, L"Connected: %s", L"\u5df2\u8fde\u63a5: %s"), g_sr6Connected.load() ? L"true" : L"false");
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_CONNECTED_LABEL), label);

    const MotionInputSnapshot input = MotionInput_GetSelected();

    swprintf_s(label, UiText(cfg, L"Inserted: %s", L"\u63d2\u5165: %s"), input.inserted ? L"true" : L"false");
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_INSERTED_LABEL), label);

    swprintf_s(
        label,
        UiText(
            cfg,
            L"L %.2f/%.2f/%.2f  R %.2f/%.2f/%.2f",
            L"L %.2f/%.2f/%.2f  R %.2f/%.2f/%.2f"),
        input.l0,
        input.l1,
        input.l2,
        input.r0,
        input.r1,
        input.r2);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_XY_LABEL), label);

    const wchar_t* suppressionState = UiText(cfg, L"disabled", L"\u5df2\u5173\u95ed");
    if (Sr6Sync_IsSyntheticClimaxPeakActive() && cfg.activeOscillationSuppression && cfg.oscillationDeadband > 0.0001f)
        suppressionState = UiText(cfg, L"bypassed during synthetic climax peak", L"\u4eba\u5de5\u9ad8\u6f6e\u5cf0\u503c\u671f\u95f4\u5df2\u7ed5\u8fc7");
    else if (input.oscillationSuppressed)
        suppressionState = UiText(cfg, L"active on native Live2D L0", L"\u6b63\u5728\u5904\u7406 Live2D \u539f\u751f L0");
    else if (cfg.activeOscillationSuppression && cfg.oscillationDeadband > 0.0001f)
        suppressionState = UiText(cfg, L"armed; waiting for a valid Live2D action", L"\u5df2\u5f85\u547d\uff0c\u7b49\u5f85\u6709\u6548 Live2D \u52a8\u4f5c");
    swprintf_s(label, UiText(cfg, L"L0 source/filter: %s", L"L0 \u6765\u6e90/\u6ee4\u6ce2: %s"), suppressionState);
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_OSCILLATION_STATE), label);

    const OrgasmSnapshot orgasm = OrgasmState_GetSnapshot();
    const unsigned long long now = GetTickCount64();
    const bool maleClimax = cfg.orgasmSync && MaleClimaxActive(orgasm, now, cfg);
    const bool femaleClimax = cfg.orgasmSync && FemaleClimaxActive(orgasm, now);
    const bool femaleAfter = cfg.orgasmSync && FemaleAfterActive(orgasm, now, cfg);
    const wchar_t* orgasmMode = UiText(cfg, L"off", L"\u5173");
    if (maleClimax && femaleClimax)
        orgasmMode = UiText(cfg, L"dual climax", L"\u53cc\u65b9\u9ad8\u6f6e");
    else if (maleClimax)
        orgasmMode = UiText(cfg, L"male climax", L"\u7537\u6027\u9ad8\u6f6e");
    else if (femaleClimax)
        orgasmMode = UiText(cfg, L"female climax", L"\u5973\u6027\u9ad8\u6f6e");
    else if (femaleAfter)
        orgasmMode = UiText(cfg, L"female after", L"\u5973\u6027\u4f59\u97f5");

    swprintf_s(
        label,
        UiText(cfg, L"Orgasm: %s  male=%s len=%.1f  after=%s", L"\u9ad8\u6f6e: %s  \u7537\u6027=%s \u65f6\u957f=%.1f  \u4f59\u97f5=%s"),
        orgasmMode,
        orgasm.maleNow ? L"true" : L"false",
        orgasm.maleLength,
        orgasm.femaleAfter ? L"true" : L"false");
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_ORGASM_STATE_LABEL), label);

    const std::wstring lastCommand = Utf8ToWide(GetSr6LastCommand());
    swprintf_s(label, UiText(cfg, L"Last command: %s", L"\u6700\u540e\u6307\u4ee4: %s"), lastCommand.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_SR6_LAST_COMMAND_LABEL), label);

    UpdateOutputTargetControlState(hwnd, cfg);

    if (forceParamRedraw)
        RedrawDeviceParamArea(hwnd);
    UpdateWindow(hwnd);
}

static void RefreshSr6PortCombo(HWND hwnd)
{
    HWND combo = GetDlgItem(hwnd, IDC_SR6_PORT);
    Sr6Config cfg = GetSr6Config();
    std::vector<std::string> ports = EnumerateSerialPorts();

    SendMessageA(combo, CB_RESETCONTENT, 0, 0);

    int selectedIndex = -1;
    for (size_t i = 0; i < ports.size(); ++i)
    {
        const int index = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ports[i].c_str())));
        if (_stricmp(ports[i].c_str(), cfg.portName.c_str()) == 0)
            selectedIndex = index;
    }

    if (selectedIndex >= 0)
    {
        SendMessageA(combo, CB_SETCURSEL, selectedIndex, 0);
    }
    else if (!cfg.portName.empty())
    {
        const int index = static_cast<int>(SendMessageA(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg.portName.c_str())));
        SendMessageA(combo, CB_SETCURSEL, index, 0);
    }
    else if (!ports.empty())
    {
        SendMessageA(combo, CB_SETCURSEL, 0, 0);
    }
}

static Sr6Config ReadSr6ConfigFromGui(HWND hwnd)
{
    Sr6Config cfg = GetSr6Config();
    cfg.enabled = GetCheck(hwnd, IDC_SR6_ENABLED);
    cfg.autoConnect = GetCheck(hwnd, IDC_SR6_AUTO);
    cfg.invertX = GetCheck(hwnd, IDC_SR6_INVERT_X);
    cfg.parkOnNotInserted = GetCheck(hwnd, IDC_SR6_PARK);
    cfg.enableSixAxis = GetCheck(hwnd, IDC_SR6_R1);
    cfg.activeOscillationSuppression = GetCheck(hwnd, IDC_SR6_OSCILLATION_ENABLED);
    cfg.orgasmSync = GetCheck(hwnd, IDC_SR6_ORGASM_ENABLED);
    cfg.outputTarget = ReadOutputTargetCombo(GetDlgItem(hwnd, IDC_SR6_OUTPUT_TARGET), cfg.outputTarget);

    char port[64]{};
    GetWindowTextA(GetDlgItem(hwnd, IDC_SR6_PORT), port, static_cast<int>(sizeof(port)));
    cfg.portName = port;
    char intifaceUrl[256]{};
    GetWindowTextA(GetDlgItem(hwnd, IDC_SR6_INTIFACE_URL), intifaceUrl, static_cast<int>(sizeof(intifaceUrl)));
    cfg.intifaceUrl = intifaceUrl;

    HWND updateSlider = GetDlgItem(hwnd, IDC_SR6_UPDATE_SLIDER);
    HWND rampSlider = GetDlgItem(hwnd, IDC_SR6_RAMP_SLIDER);
    HWND oscillationSlider = GetDlgItem(hwnd, IDC_SR6_OSCILLATION_SLIDER);
    cfg.updateMs = ClampInt(
        static_cast<int>(SendMessageA(updateSlider, TBM_GETPOS, 0, 0)),
        kMinUpdateMs,
        kMaxUpdateMs);
    cfg.rampMs = NormalizeRampMs(
        static_cast<int>(SendMessageA(rampSlider, TBM_GETPOS, 0, 0)),
        cfg.updateMs);
    cfg.oscillationDeadband = ClampFloat(
        static_cast<float>(SendMessageA(oscillationSlider, TBM_GETPOS, 0, 0)) / 100.0f,
        0.0f,
        0.5f);
    cfg.language = ReadLanguageCombo(GetDlgItem(hwnd, IDC_SR6_LANGUAGE), cfg.language);
    cfg.guiHotkey = ReadHotkeyCombo(GetDlgItem(hwnd, IDC_SR6_GUI_HOTKEY), cfg.guiHotkey);
    cfg.consoleHotkey = ReadHotkeyCombo(GetDlgItem(hwnd, IDC_SR6_CONSOLE_HOTKEY), cfg.consoleHotkey);
    cfg.orgasmClimaxHz = ReadIntEdit(hwnd, IDC_SR6_ORGASM_CLIMAX_HZ, cfg.orgasmClimaxHz, 1, kMaxPatternHz);
    cfg.orgasmAfterHz = ReadIntEdit(hwnd, IDC_SR6_ORGASM_AFTER_HZ, cfg.orgasmAfterHz, 1, kMaxPatternHz);
    cfg.intifaceDeviceIndex = ReadIntifaceDeviceCombo(GetDlgItem(hwnd, IDC_SR6_INTIFACE_DEVICE), cfg.intifaceDeviceIndex);
    GetRangeSliderValues(GetDlgItem(hwnd, IDC_SR6_ORGASM_L_RANGE), cfg.orgasmLMin, cfg.orgasmLMax);
    GetRangeSliderValues(GetDlgItem(hwnd, IDC_SR6_ORGASM_R_RANGE), cfg.orgasmRMin, cfg.orgasmRMax);

    SetTrackbarRange(rampSlider, 0, cfg.updateMs);
    SendMessageA(rampSlider, TBM_SETPOS, TRUE, cfg.rampMs);
    return cfg;
}

static void ApplySr6ConfigToGui(HWND hwnd)
{
    Sr6Config cfg = GetSr6Config();
    SetCheck(hwnd, IDC_SR6_ENABLED, cfg.enabled);
    SetCheck(hwnd, IDC_SR6_AUTO, cfg.autoConnect);
    SetCheck(hwnd, IDC_SR6_INVERT_X, cfg.invertX);
    SetCheck(hwnd, IDC_SR6_PARK, cfg.parkOnNotInserted);
    SetCheck(hwnd, IDC_SR6_R1, cfg.enableSixAxis);
    SetCheck(hwnd, IDC_SR6_OSCILLATION_ENABLED, cfg.activeOscillationSuppression);
    SetCheck(hwnd, IDC_SR6_ORGASM_ENABLED, cfg.orgasmSync);

    HWND updateSlider = GetDlgItem(hwnd, IDC_SR6_UPDATE_SLIDER);
    HWND rampSlider = GetDlgItem(hwnd, IDC_SR6_RAMP_SLIDER);
    HWND oscillationSlider = GetDlgItem(hwnd, IDC_SR6_OSCILLATION_SLIDER);
    HWND lRange = GetDlgItem(hwnd, IDC_SR6_ORGASM_L_RANGE);
    HWND rRange = GetDlgItem(hwnd, IDC_SR6_ORGASM_R_RANGE);
    PopulateOutputTargetCombo(GetDlgItem(hwnd, IDC_SR6_OUTPUT_TARGET), cfg.outputTarget);
    SetWindowTextA(GetDlgItem(hwnd, IDC_SR6_INTIFACE_URL), cfg.intifaceUrl.c_str());
    PopulateIntifaceDeviceCombo(GetDlgItem(hwnd, IDC_SR6_INTIFACE_DEVICE), cfg.intifaceDeviceIndex);
    PopulateLanguageCombo(GetDlgItem(hwnd, IDC_SR6_LANGUAGE), cfg.language);
    PopulateHotkeyCombo(GetDlgItem(hwnd, IDC_SR6_GUI_HOTKEY), cfg.guiHotkey);
    PopulateHotkeyCombo(GetDlgItem(hwnd, IDC_SR6_CONSOLE_HOTKEY), cfg.consoleHotkey);
    SetTrackbarRange(updateSlider, kMinUpdateMs, kMaxUpdateMs);
    SetTrackbarRange(
        rampSlider,
        MinimumSmoothRampMs(cfg.updateMs),
        ClampInt(cfg.updateMs, kMinUpdateMs, kMaxUpdateMs));
    SendMessageA(updateSlider, TBM_SETPOS, TRUE, ClampInt(cfg.updateMs, kMinUpdateMs, kMaxUpdateMs));
    SendMessageA(rampSlider, TBM_SETPOS, TRUE, ClampInt(cfg.rampMs, 0, cfg.updateMs));
    SetTrackbarRange(oscillationSlider, 0, 50);
    SendMessageA(
        oscillationSlider,
        TBM_SETPOS,
        TRUE,
        static_cast<LPARAM>(ClampInt(static_cast<int>(std::lround(cfg.oscillationDeadband * 100.0f)), 0, 50)));
    SetRangeSliderValues(lRange, cfg.orgasmLMin, cfg.orgasmLMax);
    SetRangeSliderValues(rRange, cfg.orgasmRMin, cfg.orgasmRMax);
    SetDlgItemInt(hwnd, IDC_SR6_ORGASM_CLIMAX_HZ, static_cast<UINT>(ClampInt(cfg.orgasmClimaxHz, 1, kMaxPatternHz)), FALSE);
    SetDlgItemInt(hwnd, IDC_SR6_ORGASM_AFTER_HZ, static_cast<UINT>(ClampInt(cfg.orgasmAfterHz, 1, kMaxPatternHz)), FALSE);

    RefreshSr6PortCombo(hwnd);
    ApplyLanguageToGui(hwnd);
    UpdateSr6GuiLabels(hwnd, true);
}

static void SaveSr6ConfigFromGui(HWND hwnd, bool saveToDisk = true)
{
    Sr6Config cfg = ReadSr6ConfigFromGui(hwnd);
    UpdateSr6Config(cfg, saveToDisk);
    UpdateSr6GuiLabels(hwnd, true);
}

struct IntifaceGuiAsyncRequest
{
    HWND hwnd = nullptr;
    Sr6Config cfg;
};

static DWORD WINAPI IntifaceGuiScanThread(LPVOID param)
{
    IntifaceGuiAsyncRequest* request = reinterpret_cast<IntifaceGuiAsyncRequest*>(param);
    HWND hwnd = request ? request->hwnd : nullptr;
    Sr6Config cfg = request ? request->cfg : Sr6Config{};
    delete request;

    std::string error;
    const bool ok = RefreshIntifaceDeviceList(cfg, error);
    SetSr6Status(ok ? "Intiface devices refreshed" : error);
    g_intifaceGuiScanRunning.store(false);

    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_INTIFACE_SCAN_DONE, ok ? 1 : 0, 0);
    return 0;
}

static DWORD WINAPI IntifaceGuiTestThread(LPVOID param)
{
    IntifaceGuiAsyncRequest* request = reinterpret_cast<IntifaceGuiAsyncRequest*>(param);
    HWND hwnd = request ? request->hwnd : nullptr;
    Sr6Config cfg = request ? request->cfg : Sr6Config{};
    delete request;

    std::string error;
    const bool ok = TestIntifaceOutput(cfg, error);
    SetSr6Status(ok ? "Intiface test sent" : error);
    g_intifaceGuiTestRunning.store(false);

    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_INTIFACE_TEST_DONE, ok ? 1 : 0, 0);
    return 0;
}

static bool StartIntifaceGuiAsync(HWND hwnd, const Sr6Config& cfg, bool test)
{
    std::atomic<bool>* running = test ? &g_intifaceGuiTestRunning : &g_intifaceGuiScanRunning;
    if (running->exchange(true))
    {
        SetSr6Status(test ? "Intiface test already running" : "Intiface scan already running");
        return false;
    }

    IntifaceGuiAsyncRequest* request = new IntifaceGuiAsyncRequest();
    request->hwnd = hwnd;
    request->cfg = cfg;

    HANDLE thread = CreateThread(
        nullptr,
        0,
        test ? IntifaceGuiTestThread : IntifaceGuiScanThread,
        request,
        0,
        nullptr);
    if (!thread)
    {
        delete request;
        running->store(false);
        SetSr6Status(std::string(test ? "Intiface test thread failed code=" : "Intiface scan thread failed code=") + std::to_string(GetLastError()));
        return false;
    }

    CloseHandle(thread);
    return true;
}

static void CompleteIntifaceDeviceRefreshInGui(HWND hwnd)
{
    Sr6Config cfg = GetSr6Config();
    PopulateIntifaceDeviceCombo(GetDlgItem(hwnd, IDC_SR6_INTIFACE_DEVICE), cfg.intifaceDeviceIndex);
    SaveSr6ConfigFromGui(hwnd);
    UpdateSr6GuiLabels(hwnd, true);
}

static void RefreshIntifaceDevicesFromGui(HWND hwnd)
{
    SaveSr6ConfigFromGui(hwnd);
    Sr6Config cfg = GetSr6Config();
    if (g_sr6ConnectionInProgress.load())
    {
        SetSr6Status("Wait for Intiface connection to finish");
        UpdateSr6GuiLabels(hwnd, true);
        return;
    }

    SetSr6Status("Scanning Intiface devices...");
    if (cfg.outputTarget == OutputTarget::Intiface && g_sr6Connected.load())
    {
        if (!g_intifaceGuiScanRunning.exchange(true))
            g_intifaceWorkerScanRequested.store(true);
        else
            SetSr6Status("Intiface scan already running");
    }
    else
    {
        StartIntifaceGuiAsync(hwnd, cfg, false);
    }
    UpdateSr6GuiLabels(hwnd, true);
}

static void TestIntifaceFromGui(HWND hwnd)
{
    SaveSr6ConfigFromGui(hwnd);
    Sr6Config cfg = GetSr6Config();
    if (g_sr6ConnectionInProgress.load())
    {
        SetSr6Status("Wait for Intiface connection to finish");
        UpdateSr6GuiLabels(hwnd, true);
        return;
    }

    SetSr6Status("Sending Intiface test...");
    if (cfg.outputTarget == OutputTarget::Intiface && g_sr6Connected.load())
    {
        if (!g_intifaceGuiTestRunning.exchange(true))
            g_intifaceWorkerTestRequested.store(true);
        else
            SetSr6Status("Intiface test already running");
    }
    else
    {
        StartIntifaceGuiAsync(hwnd, cfg, true);
    }
    UpdateSr6GuiLabels(hwnd, true);
}

static void ResizeControl(HWND hwnd, int id, int x, int y, int w, int h)
{
    HWND control = GetDlgItem(hwnd, id);
    if (control)
        SetWindowPos(control, nullptr, x, y, std::max(1, w), std::max(1, h), SWP_NOZORDER | SWP_NOACTIVATE);
}

static void LayoutSr6Gui(HWND hwnd)
{
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const Sr6Config cfg = GetSr6Config();
    const bool isIntiface = cfg.outputTarget == OutputTarget::Intiface;
    const int minHeight = isIntiface ? 900 : 820;
    const int width = std::max(590, static_cast<int>(rc.right - rc.left));
    const int height = std::max(minHeight, static_cast<int>(rc.bottom - rc.top));
    const int contentW = width - 24;
    const int sharedY = isIntiface ? 188 : 104;

    ResizeControl(hwnd, IDC_SR6_STATUS, 12, 12, contentW, 18);
    ResizeControl(hwnd, IDC_SR6_CONNECTED_LABEL, 12, 36, 150, 18);
    ResizeControl(hwnd, IDC_SR6_INSERTED_LABEL, 176, 36, 138, 18);
    ResizeControl(hwnd, IDC_SR6_XY_LABEL, width - 262, 36, 250, 18);

    ResizeControl(hwnd, IDC_SR6_LABEL_OUTPUT, 12, 70, 52, 18);
    ResizeControl(hwnd, IDC_SR6_OUTPUT_TARGET, 70, 66, 142, 180);
    ResizeControl(hwnd, IDC_SR6_PORT, 224, 66, std::max(90, width - 466), 220);
    ResizeControl(hwnd, IDC_SR6_REFRESH, width - 224, 66, 76, 24);
    ResizeControl(hwnd, IDC_SR6_CONNECT, width - 138, 66, 58, 24);
    ResizeControl(hwnd, IDC_SR6_DISCONNECT, width - 72, 66, 72, 24);

    ResizeControl(hwnd, IDC_SR6_INTIFACE_GROUP, 12, 98, contentW, 78);
    ResizeControl(hwnd, IDC_SR6_LABEL_INTIFACE, 24, 116, 58, 18);
    ResizeControl(hwnd, IDC_SR6_INTIFACE_URL, 82, 112, std::max(160, width - 254), 22);
    ResizeControl(hwnd, IDC_SR6_INTIFACE_SCAN, width - 158, 112, 70, 24);
    ResizeControl(hwnd, IDC_SR6_INTIFACE_TEST, width - 80, 112, 68, 24);
    ResizeControl(hwnd, IDC_SR6_LABEL_DEVICE, 24, 148, 46, 18);
    ResizeControl(hwnd, IDC_SR6_INTIFACE_DEVICE, 82, 144, std::max(180, width - 106), 160);

    ResizeControl(hwnd, IDC_SR6_ENABLED, 12, sharedY, 110, 22);
    ResizeControl(hwnd, IDC_SR6_AUTO, 136, sharedY, 120, 22);
    ResizeControl(hwnd, IDC_SR6_R1, 276, sharedY, 120, 22);
    ResizeControl(hwnd, IDC_SR6_PARK, isIntiface ? 276 : 136, isIntiface ? sharedY : sharedY + 28, 150, 22);
    ResizeControl(hwnd, IDC_SR6_INVERT_X, 12, sharedY + 28, 100, 22);

    ResizeControl(hwnd, IDC_SR6_LABEL_GUI_KEY, 12, sharedY + 64, 58, 18);
    ResizeControl(hwnd, IDC_SR6_GUI_HOTKEY, 76, sharedY + 60, 94, 220);
    ResizeControl(hwnd, IDC_SR6_LABEL_CONSOLE_KEY, 190, sharedY + 64, 82, 18);
    ResizeControl(hwnd, IDC_SR6_CONSOLE_HOTKEY, 282, sharedY + 60, 94, 220);
    ResizeControl(hwnd, IDC_SR6_LABEL_LANGUAGE, width - 194, sharedY + 64, 74, 18);
    ResizeControl(hwnd, IDC_SR6_LANGUAGE, width - 108, sharedY + 60, 96, 120);

    ResizeControl(hwnd, IDC_SR6_UPDATE_LABEL, 12, sharedY + 108, std::max(220, width - 24), 18);
    ResizeControl(hwnd, IDC_SR6_UPDATE_SLIDER, 12, sharedY + 130, contentW, 34);
    ResizeControl(hwnd, IDC_SR6_RAMP_LABEL, 12, sharedY + 172, std::max(180, width - 24), 18);
    ResizeControl(hwnd, IDC_SR6_RAMP_SLIDER, 12, sharedY + 194, contentW, 34);

    ResizeControl(hwnd, IDC_SR6_OSCILLATION_ENABLED, 12, sharedY + 234, 260, 22);
    ResizeControl(hwnd, IDC_SR6_OSCILLATION_LABEL, 12, sharedY + 262, contentW, 18);
    ResizeControl(hwnd, IDC_SR6_OSCILLATION_SLIDER, 12, sharedY + 282, contentW, 30);
    ResizeControl(hwnd, IDC_SR6_OSCILLATION_STATE, 12, sharedY + 316, contentW, 18);

    ResizeControl(hwnd, IDC_SR6_ORGASM_ENABLED, 12, sharedY + 350, 160, 22);
    ResizeControl(hwnd, IDC_SR6_LABEL_CLIMAX_HZ, 190, sharedY + 354, 68, 18);
    ResizeControl(hwnd, IDC_SR6_ORGASM_CLIMAX_HZ, 260, sharedY + 350, 44, 22);
    ResizeControl(hwnd, IDC_SR6_LABEL_AFTER_HZ, 318, sharedY + 354, 58, 18);
    ResizeControl(hwnd, IDC_SR6_ORGASM_AFTER_HZ, 380, sharedY + 350, 44, 22);

    ResizeControl(hwnd, IDC_SR6_ORGASM_L_RANGE_LABEL, 12, sharedY + 386, std::max(180, width - 24), 18);
    ResizeControl(hwnd, IDC_SR6_ORGASM_L_RANGE, 12, sharedY + 406, contentW, 34);
    ResizeControl(hwnd, IDC_SR6_ORGASM_R_RANGE_LABEL, 12, sharedY + 446, std::max(180, width - 24), 18);
    ResizeControl(hwnd, IDC_SR6_ORGASM_R_RANGE, 12, sharedY + 466, contentW, 34);
    ResizeControl(hwnd, IDC_SR6_ORGASM_STATE_LABEL, 12, sharedY + 508, contentW, 18);

    const int commandGroupY = sharedY + 536;
    ResizeControl(hwnd, IDC_SR6_COMMAND_GROUP, 12, commandGroupY, contentW, height - commandGroupY - 16);
    ResizeControl(hwnd, IDC_SR6_LAST_COMMAND_LABEL, 24, commandGroupY + 24, width - 48, 18);
    ResizeControl(hwnd, IDC_SR6_COMMAND_EDIT, 24, commandGroupY + 50, width - 48, height - commandGroupY - 66);
}

static LRESULT CALLBACK Sr6GuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_SR6_TOGGLE_GUI:
    {
        const bool visible = IsWindowVisible(hwnd) != FALSE;
        ShowWindow(hwnd, visible ? SW_HIDE : SW_SHOW);
        if (!visible)
        {
            SetForegroundWindow(hwnd);
            UpdateSr6GuiLabels(hwnd, true);
        }
        SaveVisibilityState(!visible, g_consoleVisible.load());
        return 0;
    }
    case WM_CREATE:
    {
        HFONT font = g_guiFont ? g_guiFont : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if (!g_commandEditBrush)
            g_commandEditBrush = CreateSolidBrush(kGuiCommandBgColor);
        auto addControl = [&](const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) -> HWND
        {
            HWND control = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleA(nullptr), nullptr);
            SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetWindowTheme(control, L"Explorer", nullptr);
            return control;
        };
        auto addControlEx = [&](DWORD exStyle, const char* cls, const char* text, DWORD style, int x, int y, int w, int h, int id) -> HWND
        {
            HWND control = CreateWindowExA(exStyle, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleA(nullptr), nullptr);
            SendMessageA(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            SetWindowTheme(control, L"Explorer", nullptr);
            return control;
        };

        addControl("STATIC", "Status: SR6 idle", 0, 12, 12, 548, 18, IDC_SR6_STATUS);
        addControl("STATIC", "Connected: false", 0, 12, 36, 150, 18, IDC_SR6_CONNECTED_LABEL);
        addControl("STATIC", "Inserted: false", 0, 176, 36, 138, 18, IDC_SR6_INSERTED_LABEL);
        addControl("STATIC", "Input: x=0.000  y=0.000", 0, 328, 36, 232, 18, IDC_SR6_XY_LABEL);

        addControl("STATIC", "Output", 0, 12, 70, 52, 18, IDC_SR6_LABEL_OUTPUT);
        addControl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 70, 66, 142, 180, IDC_SR6_OUTPUT_TARGET);
        addControl("COMBOBOX", "", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, 224, 66, 120, 220, IDC_SR6_PORT);
        addControl("BUTTON", "Refresh", BS_PUSHBUTTON | WS_TABSTOP, 354, 66, 76, 24, IDC_SR6_REFRESH);
        addControl("BUTTON", "Connect", BS_PUSHBUTTON | WS_TABSTOP, 440, 66, 58, 24, IDC_SR6_CONNECT);
        addControl("BUTTON", "Disconnect", BS_PUSHBUTTON | WS_TABSTOP, 506, 66, 72, 24, IDC_SR6_DISCONNECT);

        addControl("BUTTON", "Intiface Central Settings", BS_GROUPBOX, 12, 88, 558, 84, IDC_SR6_INTIFACE_GROUP);

        addControl("STATIC", "Intiface", 0, 24, 106, 58, 18, IDC_SR6_LABEL_INTIFACE);
        addControlEx(WS_EX_CLIENTEDGE, "EDIT", "ws://localhost:12345", WS_TABSTOP, 82, 102, 340, 22, IDC_SR6_INTIFACE_URL);
        addControl("BUTTON", "Scan IC", BS_PUSHBUTTON | WS_TABSTOP, 432, 102, 70, 24, IDC_SR6_INTIFACE_SCAN);
        addControl("BUTTON", "Test IC", BS_PUSHBUTTON | WS_TABSTOP, 510, 102, 68, 24, IDC_SR6_INTIFACE_TEST);
        addControl("STATIC", "Device", 0, 24, 136, 46, 18, IDC_SR6_LABEL_DEVICE);
        addControl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 82, 132, 248, 160, IDC_SR6_INTIFACE_DEVICE);

        addControl("BUTTON", "Enable sync", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 166, 110, 22, IDC_SR6_ENABLED);
        addControl("BUTTON", "Auto connect", BS_AUTOCHECKBOX | WS_TABSTOP, 136, 166, 120, 22, IDC_SR6_AUTO);
        addControl("BUTTON", "Enable SR6 6-axis", BS_AUTOCHECKBOX | WS_TABSTOP, 276, 166, 140, 22, IDC_SR6_R1);
        addControl("BUTTON", "Invert X", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 194, 100, 22, IDC_SR6_INVERT_X);
        addControl("BUTTON", "Park on notInsert", BS_AUTOCHECKBOX | WS_TABSTOP, 136, 194, 150, 22, IDC_SR6_PARK);

        addControl("STATIC", "GUI key", 0, 12, 230, 58, 18, IDC_SR6_LABEL_GUI_KEY);
        addControl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 76, 226, 94, 220, IDC_SR6_GUI_HOTKEY);
        addControl("STATIC", "Console key", 0, 190, 230, 82, 18, IDC_SR6_LABEL_CONSOLE_KEY);
        addControl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 282, 226, 94, 220, IDC_SR6_CONSOLE_HOTKEY);
        addControl("STATIC", "Language", 0, 396, 230, 74, 18, IDC_SR6_LABEL_LANGUAGE);
        addControl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 476, 226, 84, 120, IDC_SR6_LANGUAGE);

        addControl("STATIC", "Device send interval: 100 ms", 0, 12, 274, 220, 18, IDC_SR6_UPDATE_LABEL);
        addControl(TRACKBAR_CLASSA, "", TBS_AUTOTICKS | WS_TABSTOP, 12, 296, 548, 34, IDC_SR6_UPDATE_SLIDER);
        addControl("STATIC", "I ramp: 90 ms", 0, 12, 338, 160, 18, IDC_SR6_RAMP_LABEL);
        addControl(TRACKBAR_CLASSA, "", TBS_AUTOTICKS | WS_TABSTOP, 12, 360, 548, 34, IDC_SR6_RAMP_SLIDER);

        addControl("BUTTON", "Active L0 oscillation suppression", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 398, 260, 22, IDC_SR6_OSCILLATION_ENABLED);
        addControl("STATIC", "Oscillation deadband: 50%", 0, 12, 426, 548, 18, IDC_SR6_OSCILLATION_LABEL);
        addControl(TRACKBAR_CLASSA, "", TBS_AUTOTICKS | WS_TABSTOP, 12, 446, 548, 30, IDC_SR6_OSCILLATION_SLIDER);
        addControl("STATIC", "L0 source/filter: armed", 0, 12, 480, 548, 18, IDC_SR6_OSCILLATION_STATE);

        addControl("BUTTON", "Enable orgasm sync", BS_AUTOCHECKBOX | WS_TABSTOP, 12, 514, 160, 22, IDC_SR6_ORGASM_ENABLED);
        addControl("STATIC", "Climax Hz", 0, 190, 518, 68, 18, IDC_SR6_LABEL_CLIMAX_HZ);
        addControlEx(WS_EX_CLIENTEDGE, "EDIT", "10", ES_NUMBER | WS_TABSTOP, 260, 514, 44, 22, IDC_SR6_ORGASM_CLIMAX_HZ);
        addControl("STATIC", "After Hz", 0, 318, 518, 58, 18, IDC_SR6_LABEL_AFTER_HZ);
        addControlEx(WS_EX_CLIENTEDGE, "EDIT", "1", ES_NUMBER | WS_TABSTOP, 380, 514, 44, 22, IDC_SR6_ORGASM_AFTER_HZ);

        addControl("STATIC", "L range: 400 - 600", 0, 12, 550, 180, 18, IDC_SR6_ORGASM_L_RANGE_LABEL);
        addControl(kRangeSliderClassName, "", WS_TABSTOP, 12, 570, 548, 34, IDC_SR6_ORGASM_L_RANGE);
        addControl("STATIC", "R range: 400 - 600", 0, 12, 610, 180, 18, IDC_SR6_ORGASM_R_RANGE_LABEL);
        addControl(kRangeSliderClassName, "", WS_TABSTOP, 12, 630, 548, 34, IDC_SR6_ORGASM_R_RANGE);
        addControl("STATIC", "Orgasm: off", 0, 12, 672, 548, 18, IDC_SR6_ORGASM_STATE_LABEL);

        addControl("BUTTON", "Commands sent to selected output", BS_GROUPBOX, 12, 698, 548, 154, IDC_SR6_COMMAND_GROUP);
        addControl("STATIC", "Last command: (none)", 0, 24, 722, 524, 18, IDC_SR6_LAST_COMMAND_LABEL);
        HWND commandEdit = addControlEx(
            WS_EX_CLIENTEDGE,
            "EDIT",
            "(no commands sent yet)\r\n",
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
            24,
            748,
            524,
            88,
            IDC_SR6_COMMAND_EDIT);
        SendMessageA(commandEdit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(ANSI_FIXED_FONT)), TRUE);

        ApplySr6ConfigToGui(hwnd);
        LayoutSr6Gui(hwnd);
        RefreshCommandDisplay(hwnd, true);
        SetTimer(hwnd, 1, 200, nullptr);
        return 0;
    }
    case WM_SIZE:
        LayoutSr6Gui(hwnd);
        UpdateSr6GuiLabels(hwnd, true);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const Sr6Config cfg = GetSr6Config();
        info->ptMinTrackSize.x = 590;
        info->ptMinTrackSize.y = cfg.outputTarget == OutputTarget::Intiface ? 930 : 850;
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int notify = HIWORD(wParam);
        if (id == IDC_SR6_REFRESH && notify == BN_CLICKED)
        {
            SaveSr6ConfigFromGui(hwnd);
            Sr6Config cfg = GetSr6Config();
            if (cfg.outputTarget == OutputTarget::Intiface)
            {
                RefreshIntifaceDevicesFromGui(hwnd);
            }
            else
            {
                RefreshSr6PortCombo(hwnd);
                SaveSr6ConfigFromGui(hwnd);
            }
            UpdateSr6GuiLabels(hwnd);
            return 0;
        }
        if (id == IDC_SR6_INTIFACE_SCAN && notify == BN_CLICKED)
        {
            RefreshIntifaceDevicesFromGui(hwnd);
            return 0;
        }
        if (id == IDC_SR6_INTIFACE_TEST && notify == BN_CLICKED)
        {
            TestIntifaceFromGui(hwnd);
            return 0;
        }
        if (id == IDC_SR6_CONNECT && notify == BN_CLICKED)
        {
            SaveSr6ConfigFromGui(hwnd);
            Sr6Config cfg = GetSr6Config();
            g_sr6ConnectRequested.store(true);
            SetSr6Status(cfg.outputTarget == OutputTarget::Intiface
                ? "Intiface connect requested..."
                : "serial connect requested...");
            UpdateSr6GuiLabels(hwnd, true);
            // Immediate button state feedback
            UpdateOutputTargetControlState(hwnd, cfg);
            return 0;
        }
        if (id == IDC_SR6_DISCONNECT && notify == BN_CLICKED)
        {
            g_sr6DisconnectRequested.store(true);
            SetSr6Status("disconnect requested");
            UpdateSr6GuiLabels(hwnd, true);
            // Immediate button state feedback
            UpdateOutputTargetControlState(hwnd, GetSr6Config());
            return 0;
        }
        if ((id == IDC_SR6_ENABLED || id == IDC_SR6_AUTO || id == IDC_SR6_INVERT_X || id == IDC_SR6_PARK || id == IDC_SR6_R1 || id == IDC_SR6_ORGASM_ENABLED || id == IDC_SR6_OSCILLATION_ENABLED) && notify == BN_CLICKED)
        {
            SaveSr6ConfigFromGui(hwnd);
            return 0;
        }
        if (id == IDC_SR6_PORT && notify == CBN_SELCHANGE)
        {
            SaveSr6ConfigFromGui(hwnd);
            return 0;
        }
        if ((id == IDC_SR6_GUI_HOTKEY ||
             id == IDC_SR6_CONSOLE_HOTKEY ||
             id == IDC_SR6_OUTPUT_TARGET ||
             id == IDC_SR6_INTIFACE_DEVICE ||
             id == IDC_SR6_LANGUAGE) &&
            notify == CBN_SELCHANGE)
        {
            SaveSr6ConfigFromGui(hwnd);
            ApplyLanguageToGui(hwnd);
            LayoutSr6Gui(hwnd);
            UpdateSr6GuiLabels(hwnd, true);
            return 0;
        }
        if ((id == IDC_SR6_ORGASM_CLIMAX_HZ ||
             id == IDC_SR6_ORGASM_AFTER_HZ ||
             id == IDC_SR6_INTIFACE_URL) &&
            notify == EN_KILLFOCUS)
        {
            SaveSr6ConfigFromGui(hwnd);
            return 0;
        }
        break;
    }
    case WM_HSCROLL:
    {
        const int scrollCode = LOWORD(wParam);
        if (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION)
            SaveSr6ConfigFromGui(hwnd);
        else
            SaveSr6ConfigFromGui(hwnd, false);
        const Sr6Config cfg = GetSr6Config();
        HWND rampSlider = GetDlgItem(hwnd, IDC_SR6_RAMP_SLIDER);
        SetTrackbarRange(rampSlider, MinimumSmoothRampMs(cfg.updateMs), cfg.updateMs);
        SendMessageA(rampSlider, TBM_SETPOS, TRUE, cfg.rampMs);
        UpdateSr6GuiLabels(hwnd, true);
        return 0;
    }
    case WM_RANGE_SLIDER_CHANGED:
        SaveSr6ConfigFromGui(hwnd, lParam != 0);
        return 0;
    case WM_SR6_COMMAND_HISTORY_CHANGED:
        RefreshCommandDisplay(hwnd, true);
        UpdateSr6GuiLabels(hwnd);
        return 0;
    case WM_SR6_STATUS_CHANGED:
        UpdateSr6GuiLabels(hwnd, true);
        return 0;
    case WM_SR6_INTIFACE_SCAN_DONE:
        if (wParam)
            CompleteIntifaceDeviceRefreshInGui(hwnd);
        else
            UpdateSr6GuiLabels(hwnd, true);
        return 0;
    case WM_SR6_INTIFACE_TEST_DONE:
        if (wParam)
            PopulateIntifaceDeviceCombo(GetDlgItem(hwnd, IDC_SR6_INTIFACE_DEVICE), GetSr6Config().intifaceDeviceIndex);
        UpdateSr6GuiLabels(hwnd, true);
        return 0;
    case WM_CTLCOLOREDIT:
    {
        HWND control = reinterpret_cast<HWND>(lParam);
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (GetDlgCtrlID(control) == IDC_SR6_COMMAND_EDIT && g_commandEditBrush)
        {
            SetTextColor(dc, kGuiCommandTextColor);
            SetBkColor(dc, kGuiCommandBgColor);
            return reinterpret_cast<LRESULT>(g_commandEditBrush);
        }

        SetTextColor(dc, kGuiTextColor);
        SetBkColor(dc, kGuiEditBgColor);
        return reinterpret_cast<LRESULT>(g_editBrush ? g_editBrush : g_guiBgBrush);
    }
    case WM_CTLCOLORSTATIC:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kGuiTextColor);
        SetBkColor(dc, kGuiBgColor);
        return reinterpret_cast<LRESULT>(g_guiBgBrush);
    }
    case WM_CTLCOLORLISTBOX:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kGuiTextColor);
        SetBkColor(dc, kGuiEditBgColor);
        return reinterpret_cast<LRESULT>(g_editBrush ? g_editBrush : g_guiBgBrush);
    }
    case WM_CTLCOLORBTN:
    {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kGuiTextColor);
        SetBkColor(dc, kGuiBgColor);
        return reinterpret_cast<LRESULT>(g_guiBgBrush);
    }
    case WM_TIMER:
        UpdateSr6GuiLabels(hwnd);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        SaveVisibilityState(false, g_consoleVisible.load());
        return 0;
    case WM_SR6_QUIT:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_guiWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static DWORD WINAPI Sr6GuiThread(LPVOID)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    HINSTANCE instance = GetModuleHandleA(nullptr);
    if (!g_guiBgBrush)
        g_guiBgBrush = CreateSolidBrush(kGuiBgColor);
    if (!g_editBrush)
        g_editBrush = CreateSolidBrush(kGuiEditBgColor);
    if (!g_guiFont)
    {
        g_guiFont = CreateFontW(
            -14,
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = Sr6GuiWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = "MocaLoveReliveSr6SyncWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g_guiBgBrush ? g_guiBgBrush : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&wc);

    WNDCLASSA rangeWc{};
    rangeWc.lpfnWndProc = RangeSliderWndProc;
    rangeWc.hInstance = instance;
    rangeWc.lpszClassName = kRangeSliderClassName;
    rangeWc.hCursor = LoadCursor(nullptr, IDC_SIZEWE);
    rangeWc.hbrBackground = g_guiBgBrush ? g_guiBgBrush : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassA(&rangeWc);

    HWND hwnd = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        "Moka Love Relive SR6 Sync",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_THICKFRAME,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        590,
        980,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!hwnd)
    {
        SetSr6Status("GUI create failed code=" + std::to_string(GetLastError()));
        return 0;
    }

    g_guiWindow = hwnd;
    ShowWindow(hwnd, GetSr6Config().guiVisible ? SW_SHOW : SW_HIDE);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}

static bool IsHotkeyDown(int vk)
{
    return vk != 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void ToggleSr6GuiWindow()
{
    HWND hwnd = g_guiWindow;
    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_TOGGLE_GUI, 0, 0);
}

static void ToggleConsoleWindow()
{
    HWND console = g_consoleWindow ? g_consoleWindow : GetConsoleWindow();
    if (!console || !IsWindow(console))
        return;

    const bool visible = IsWindowVisible(console) != FALSE;
    ShowWindow(console, visible ? SW_HIDE : SW_SHOW);
    if (!visible)
        SetForegroundWindow(console);

    g_consoleVisible.store(!visible);
    SaveVisibilityState(g_guiWindow && IsWindow(g_guiWindow) && IsWindowVisible(g_guiWindow), !visible);
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    bool guiWasDown = false;
    bool consoleWasDown = false;

    while (g_sr6Running.load())
    {
        const Sr6Config cfg = GetSr6Config();
        const bool guiDown = IsHotkeyDown(cfg.guiHotkey);
        const bool consoleDown = IsHotkeyDown(cfg.consoleHotkey);

        if (guiDown && !guiWasDown)
        {
            ToggleSr6GuiWindow();
            std::cout << "[Hotkey] GUI toggle by " << HotkeyName(cfg.guiHotkey) << std::endl;
        }

        if (consoleDown && !consoleWasDown)
        {
            ToggleConsoleWindow();
            std::cout << "[Hotkey] console toggle by " << HotkeyName(cfg.consoleHotkey) << std::endl;
        }

        guiWasDown = guiDown;
        consoleWasDown = consoleDown;
        Sleep(50);
    }

    return 0;
}

void Sr6Sync_SetConsoleWindow(HWND window)
{
    g_consoleWindow = window;
    g_consoleVisible.store(window && IsWindowVisible(window));
}
void Sr6Sync_Start()
{
    g_sr6Running.store(true);
    LoadSr6Config();
    Sr6Config cfg = GetSr6Config();
    HWND console = g_consoleWindow ? g_consoleWindow : GetConsoleWindow();
    if (console && IsWindow(console))
    {
        ShowWindow(console, cfg.consoleVisible ? SW_SHOW : SW_HIDE);
        g_consoleVisible.store(cfg.consoleVisible);
    }
    CreateThread(nullptr, 0, Sr6WorkerThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, Sr6GuiThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
}
void Sr6Sync_Stop()
{
    g_sr6Running.store(false);
    g_sr6DisconnectRequested.store(true);
    HWND hwnd = g_guiWindow;
    if (hwnd && IsWindow(hwnd))
        PostMessageA(hwnd, WM_SR6_QUIT, 0, 0);
    if (g_commandEditBrush)
    {
        DeleteObject(g_commandEditBrush);
        g_commandEditBrush = nullptr;
    }
    if (g_guiBgBrush)
    {
        DeleteObject(g_guiBgBrush);
        g_guiBgBrush = nullptr;
    }
    if (g_editBrush)
    {
        DeleteObject(g_editBrush);
        g_editBrush = nullptr;
    }
    if (g_guiFont)
    {
        DeleteObject(g_guiFont);
        g_guiFont = nullptr;
    }
}
