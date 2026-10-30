#include "companion/companion_feed.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace Companion
{
namespace
{
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\MetasequoiaImeCompanion";

std::mutex g_mutex;
std::condition_variable g_wake;
std::thread g_thread;
std::atomic<bool> g_running{false};
bool g_candidate_visible = false;
RECT g_candidate{};
bool g_recording = false;
unsigned long long g_revision = 1;

std::string Snapshot()
{
    std::string line = "{\"type\":\"state\",\"candidate\":{\"visible\":";
    line += g_candidate_visible ? "true" : "false";
    line += ",\"x\":" + std::to_string(g_candidate.left) + ",\"y\":" + std::to_string(g_candidate.top) +
            ",\"w\":" + std::to_string(g_candidate.right - g_candidate.left) +
            ",\"h\":" + std::to_string(g_candidate.bottom - g_candidate.top) + "},\"voice\":{\"recording\":";
    line += g_recording ? "true" : "false";
    line += "}}\n";
    return line;
}

bool WriteAll(HANDLE pipe, const std::string &data)
{
    DWORD written = 0;
    return WriteFile(pipe, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
}

bool WaitForClient(HANDLE pipe)
{
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (ConnectNamedPipe(pipe, &overlapped))
        connected = true;
    else if (GetLastError() == ERROR_PIPE_CONNECTED)
        connected = true;
    else if (GetLastError() == ERROR_IO_PENDING)
    {
        while (g_running)
        {
            if (WaitForSingleObject(overlapped.hEvent, 250) == WAIT_OBJECT_0)
            {
                connected = true;
                break;
            }
        }
        if (!connected)
            CancelIoEx(pipe, &overlapped);
    }
    CloseHandle(overlapped.hEvent);
    return connected && g_running;
}

void Run()
{
    while (g_running)
    {
        // Per-user pipe: the default DACL already restricts it to the creator, SYSTEM and administrators.
        HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 0, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_wake.wait_for(lock, std::chrono::seconds(5), [] { return !g_running.load(); });
            continue;
        }
        if (WaitForClient(pipe))
        {
            unsigned long long sent = 0;
            while (g_running)
            {
                std::string line;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait_for(lock, std::chrono::seconds(2), [&] { return !g_running || g_revision != sent; });
                    if (!g_running)
                        break;
                    // Re-sending the snapshot every two seconds doubles as a liveness probe for a closed client.
                    sent = g_revision;
                    line = Snapshot();
                }
                if (!WriteAll(pipe, line))
                    break;
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

// Caller holds g_mutex.
void Notify()
{
    ++g_revision;
    g_wake.notify_all();
}
} // namespace

void Start()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running.exchange(true))
        return;
    g_thread = std::thread(Run);
}

void Stop()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running.exchange(false))
            return;
    }
    g_wake.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

void SetCandidate(bool visible, const RECT &bounds)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_candidate_visible == visible && (!visible || EqualRect(&g_candidate, &bounds)))
        return;
    g_candidate_visible = visible;
    if (visible)
        g_candidate = bounds;
    Notify();
}

void SetVoiceRecording(bool recording)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_recording == recording)
        return;
    g_recording = recording;
    Notify();
}
} // namespace Companion
