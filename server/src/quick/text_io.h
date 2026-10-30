#pragma once
// Moving text in and out of whatever app has focus. Every function here is safe on any thread; the
// ones that press keys sleep briefly, so call them from a worker, not from the UI thread.

#include <Windows.h>
#include <string>
#include <vector>

namespace Quick::TextIo
{
// A copy of everything on the clipboard that is stored in global memory, so a borrowed clipboard can be
// put back exactly. Bitmap handles are skipped: Windows synthesizes them from the DIB that is kept.
class Snapshot
{
  public:
    static Snapshot Take();
    void Restore() const;
    bool hasText() const;

  private:
    struct Item
    {
        UINT format = 0;
        std::vector<char> bytes;
    };
    std::vector<Item> items_;
};

std::wstring ClipboardText();
// Replaces the clipboard with plain text and returns whether it succeeded.
bool SetClipboardText(const std::wstring &text);
// Waits until Ctrl/Alt/Shift/Win from the hotkey are physically released (or the timeout passes), so
// the keys we synthesize are not combined with them.
void WaitModifiersReleased(int timeoutMs = 900);
void PressCtrl(WORD vk);
// The text the user has selected in the foreground app: UI Automation first, Ctrl+C second.
std::wstring ReadSelection();
// The whole content of the focused input: UI Automation first, Ctrl+A then Ctrl+C second.
std::wstring ReadWholeInput();
// Types text into the focused input through the clipboard (Ctrl+V). replaceAll selects everything first.
// Restores the previous clipboard afterwards.
bool PasteText(const std::wstring &text, bool replaceAll);
// Screen rectangle of the focused input (caret, or the focused control through UI Automation).
bool FocusRect(RECT &out);
std::wstring ProcessNameOf(HWND window);
} // namespace Quick::TextIo
