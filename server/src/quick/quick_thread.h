#pragma once
// The quick actions (screenshot OCR/translate, reply panel, clipboard panel) share one UI thread.
// Every window, the global hotkeys and the temporary keyboard hook live on it, so panels never need
// locks. Slow work (OCR, network) runs on short-lived workers and returns through Post.

#include <Windows.h>
#include <functional>

namespace Quick
{
struct KeyEvent
{
    UINT vk = 0;
    bool down = true;
    bool shift = false, ctrl = false, alt = false, win = false;
};

// A window that wants the keyboard while it is open without taking focus from the target app.
class KeyClient
{
  public:
    virtual ~KeyClient() = default;
    // Return true to swallow the key. Runs inside the low-level hook: keep it short and Post real work.
    virtual bool OnKey(const KeyEvent &event) = 0;
};

// Starts the thread once; safe to call from any thread.
void Start();
void Stop();
bool Running();
bool OnQuickThread();
// Runs fn on the quick thread. Dropped silently when the thread is not running.
void Post(std::function<void()> fn);
// Quick thread only. Passing nullptr removes the hook again.
void SetKeyClient(KeyClient *client);
KeyClient *CurrentKeyClient();
// Quick thread only: hotkeys use this window as their target.
HWND Controller();
} // namespace Quick
