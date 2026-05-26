#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <iomanip>
#include <sstream>
#include <atomic>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

struct ProcSample {
    SIZE_T privateBytes;
    SIZE_T workingSet;
    DWORD  gdiObjects;
    DWORD  handleCount;
    std::chrono::steady_clock::time_point timestamp;
};

struct SpikeEvent {
    ProcSample before;
    ProcSample after;
    DWORD dwmPid;
    DWORD foregroundPid;
    std::wstring foregroundExe;
};

std::atomic<bool> g_running(true);

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\n\nReceived stop signal, shutting down..." << std::endl;
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

bool EnableDebugPrivilege() {
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;
    
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    
    if (!LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tkp.Privileges[0].Luid)) {
        CloseHandle(hToken);
        return false;
    }
    
    tkp.PrivilegeCount = 1;
    tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    
    bool result = AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, NULL, 0);
    CloseHandle(hToken);
    
    return result && (GetLastError() == ERROR_SUCCESS);
}

bool IsElevated() {
    BOOL elevated = FALSE;
    HANDLE hToken = NULL;
    
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD size;
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    
    return elevated != FALSE;
}

std::wstring getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);
    
    std::wostringstream woss;
    woss << std::put_time(&tm, L"%Y-%m-%d %H:%M:%S");
    return woss.str();
}

void logSpikeToFile(const SpikeEvent& evt, const std::wstring& filename) {
    std::wofstream logFile;
    
    // Check if file exists to determine if we need to write header
    bool fileExists = false;
    std::ifstream checkFile(filename);
    if (checkFile.good()) {
        fileExists = true;
    }
    checkFile.close();
    
    logFile.open(filename, std::ios::app);
    if (!logFile.is_open()) return;
    
    // Write CSV header if new file
    if (!fileExists) {
        logFile << L"Timestamp,DWM_PID,Before_MB,After_MB,Delta_MB,WS_Before_MB,WS_After_MB,"
                << L"GDI_Before,GDI_After,Handles_Before,Handles_After,"
                << L"Foreground_PID,Foreground_EXE\n";
    }
    
    // Write spike data
    SIZE_T beforeMB = evt.before.privateBytes / (1024 * 1024);
    SIZE_T afterMB = evt.after.privateBytes / (1024 * 1024);
    SIZE_T deltaMB = afterMB - beforeMB;
    SIZE_T wsBefore = evt.before.workingSet / (1024 * 1024);
    SIZE_T wsAfter = evt.after.workingSet / (1024 * 1024);
    
    logFile << getTimestamp() << L","
            << evt.dwmPid << L","
            << beforeMB << L","
            << afterMB << L","
            << deltaMB << L","
            << wsBefore << L","
            << wsAfter << L","
            << evt.before.gdiObjects << L","
            << evt.after.gdiObjects << L","
            << evt.before.handleCount << L","
            << evt.after.handleCount << L","
            << evt.foregroundPid << L","
            << L"\"" << evt.foregroundExe << L"\"\n";
    
    logFile.close();
}

DWORD findProcessByName(const std::wstring& name) {
    PROCESSENTRY32W entry{ sizeof(entry) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name.c_str()) == 0) {
                CloseHandle(snap);
                return entry.th32ProcessID;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return 0;
}

bool sampleProcess(DWORD pid, ProcSample& out, bool silent = false) {
    // Try with full access first
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProc) {
        // Fallback to limited access for protected processes
        hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		std::cerr << "Retrying with PROCESS_QUERY_LIMITED_INFORMATION..." << std::endl;
        if (!hProc) {
            DWORD error = GetLastError();
            if (!silent) {
                std::cerr << "OpenProcess failed with error code: " << error << std::endl;
                if (error == 5) {
                    std::cerr << "Error 5 = Access Denied. Run as Administrator!" << std::endl;
                } else if (error == 87) {
                    std::cerr << "Error 87 = Invalid Parameter. Process may have terminated." << std::endl;
                }
            }
            return false;
        }
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (!GetProcessMemoryInfo(hProc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        DWORD error = GetLastError();
        if (!silent) {
            std::cerr << "GetProcessMemoryInfo failed with error code: " << error << std::endl;
        }
        CloseHandle(hProc);
        return false;
    }

    out.privateBytes = pmc.PrivateUsage;
    out.workingSet = pmc.WorkingSetSize;
    out.gdiObjects = GetGuiResources(hProc, GR_GDIOBJECTS);
    GetProcessHandleCount(hProc, &out.handleCount);
    out.timestamp = std::chrono::steady_clock::now();

    CloseHandle(hProc);
    return true;
}

DWORD getForegroundPID(std::wstring& exeNameOut) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return 0;
	std::cout << "Got foreground window handle: " << hwnd << std::endl;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        wchar_t buf[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, buf, &size)) {
            exeNameOut = buf;
        }
        CloseHandle(hProc);
    }

    return pid;
}

int main() {
    const std::wstring DWM_NAME = L"dwm.exe";
    const SIZE_T SPIKE_THRESHOLD = 1000ull * 1024 * 1024; // 1000 MB jump
    const std::wstring LOG_FILE = L"dwm_spikes.csv";

    std::cout << "=== DWM Memory Spike Monitor ===" << std::endl;
    
    if (IsElevated()) {
        std::cout << "[OK] Running with Administrator privileges" << std::endl;
    } else {
        std::cerr << "[WARNING] NOT running as Administrator!" << std::endl;
        std::cout << "Right-click the .exe and select 'Run as administrator'" << std::endl;
        std::cout << "\nPress any key to exit...";
        std::cin.get();
        return 1;
    }

    std::cout << "Enabling SeDebugPrivilege... ";
    if (EnableDebugPrivilege()) {
        std::cout << "[OK]" << std::endl;
    } else {
        std::cerr << "[FAILED - may have limited access]" << std::endl;
    }

    if (!SetConsoleCtrlHandler(ConsoleHandler, TRUE)) {
        std::cerr << "Warning: Could not set control handler" << std::endl;
    }

    DWORD dwmPid = findProcessByName(DWM_NAME);
    if (!dwmPid) {
        std::cerr << "Failed to find dwm.exe process!" << std::endl;
        std::cout << "Press Enter to exit...";
        std::cin.get();
        return 1;
    }

    std::cout << "Found DWM process (PID: " << dwmPid << ")" << std::endl;
    std::cout << "Attempting to access process memory..." << std::endl;

    ProcSample prev{}, curr{};
    if (!sampleProcess(dwmPid, prev)) {
        std::cerr << "\n[ERROR] Failed to read DWM memory!" << std::endl;
        std::cerr << "\nThis can happen because:" << std::endl;
        std::cerr << "1. DWM is a protected process on Windows 10/11" << std::endl;
        std::cerr << "2. Some Windows security policies block access" << std::endl;
        std::cerr << "3. Antivirus/EDR software is blocking process access" << std::endl;
        std::cerr << "\nTry:" << std::endl;
        std::cerr << "- Running from an Administrator Command Prompt" << std::endl;
        std::cerr << "- Temporarily disabling antivirus" << std::endl;
        std::cerr << "- Checking Windows Defender Application Control policies" << std::endl;
        std::cout << "\nPress any key to exit...";
        std::cin.get();
        return 1;
    }

    std::cout << "[OK] Successfully accessed DWM memory!" << std::endl;
    std::cout << "Initial memory: " << (prev.privateBytes / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "\nMonitoring DWM (PID " << dwmPid << ")" << std::endl;
    std::wcout << L"Logging to: " << LOG_FILE << std::endl;
    std::cout << "Threshold: " << (SPIKE_THRESHOLD / (1024 * 1024)) << " MB" << std::endl;
    std::cout << "Press Ctrl+C to stop monitoring...\n" << std::endl;

    int failureCount = 0;
    const int MAX_FAILURES = 5;

	auto lastDisplayTime = std::chrono::steady_clock::now();
	const auto displayInterval = std::chrono::milliseconds(1000);

	bool hasAlerted = false;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (!sampleProcess(dwmPid, curr, false)) {
            failureCount++;
            
            if (failureCount >= MAX_FAILURES) {
                std::cerr << "\n[WARNING] Lost connection to DWM - checking if it restarted..." << std::endl;
                
                DWORD newPid = findProcessByName(DWM_NAME);
                if (newPid && newPid != dwmPid) {
                    std::cout << "[INFO] DWM restarted! Old PID: " << dwmPid << ", New PID: " << newPid << std::endl;
                    dwmPid = newPid;
                    
                    if (sampleProcess(dwmPid, prev)) {
                        std::cout << "[OK] Reconnected to DWM. New baseline: " 
                                  << (prev.privateBytes / (1024 * 1024)) << " MB" << std::endl;
                        failureCount = 0;
                        continue;
                    }
                }
                
                std::cerr << "[ERROR] Unable to reconnect to DWM. Exiting..." << std::endl;
                break;
            }
            continue;
        }

        failureCount = 0;

        SIZE_T delta = 0;
        if (curr.privateBytes > prev.privateBytes)
            delta = curr.privateBytes - prev.privateBytes;

		auto now = std::chrono::steady_clock::now();
        if (now - lastDisplayTime >= displayInterval) {
            std::cout << "\rCurrent Memory feed: " << (curr.privateBytes / (1024 * 1024)) << " MB   " << std::flush;
            std::cout << "Previous memory feed: " << (prev.privateBytes / (1024 * 1024)) << " MB   " << std::flush;
            //std::cout << "Delta: " << (delta / (1024 * 1024)) << " MB   " << std::flush;
            std::cout << "Threshold: " << (SPIKE_THRESHOLD / (1024 * 1024)) << " MB   ";
            lastDisplayTime = now;
        }

        if (curr.privateBytes >= SPIKE_THRESHOLD && !hasAlerted) {
            SpikeEvent evt{};
            evt.before = prev;
            evt.after = curr;
            evt.dwmPid = dwmPid;
            evt.foregroundPid = getForegroundPID(evt.foregroundExe);

            std::wcout << L"\n=== DWM MEMORY STATE JUMP DETECTED ===\n";
            std::wcout << L"PrivateBytes: "
                << (prev.privateBytes / (1024 * 1024)) << L" MB -> "
                << (curr.privateBytes / (1024 * 1024)) << L" MB\n";
            std::wcout << L"Foreground PID: " << evt.foregroundPid << L"\n";
            std::wcout << L"Foreground EXE: " << evt.foregroundExe << L"\n";
            std::wcout << L"===================================\n";

            logSpikeToFile(evt, LOG_FILE);
			hasAlerted = true;
            prev = curr;
        }
        else if (curr.privateBytes < SPIKE_THRESHOLD && hasAlerted) {
            hasAlerted = false;
        }
    }    

    std::cout << "\nMonitoring stopped. Log saved to dwm_spikes.csv" << std::endl;
    return 0;
}
