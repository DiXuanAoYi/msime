#include "Composition/NativeCommitUndo.h"
#include <iostream>

int main()
{
    const auto edit = CreateWindowExW(0, L"EDIT", L"", WS_POPUP | ES_MULTILINE, 0, 0, 400, 100, nullptr, nullptr,
                                      GetModuleHandleW(nullptr), nullptr);
    if (!edit)
        return 1;
    int failures = 0;
    const auto check = [&](bool pass, const char *name) {
        if (!pass)
        {
            ++failures;
            std::cerr << name << '\n';
        }
    };
    const auto read = [&] {
        wchar_t text[100]{};
        GetWindowTextW(edit, text, 100);
        return std::wstring(text);
    };
    const auto seed = [&] {
        SetWindowTextW(edit, L"前缀你好");
        SendMessageW(edit, EM_SETSEL, 4, 4);
    };
    NativeCommitUndo::Snapshot snapshot;
    seed();
    snapshot.Arm(edit, L"你好");
    check(snapshot.Capture(edit), "capture committed suffix");
    check(snapshot.Undo(edit, [] { return true; }), "undo committed suffix");
    check(read() == L"前缀", "preserve preceding text");
    check(!snapshot.Undo(edit, [] { return true; }), "consume once");
    seed();
    snapshot.Arm(edit, L"你好");
    snapshot.Capture(edit);
    SetWindowTextW(edit, L"前缀再见");
    SendMessageW(edit, EM_SETSEL, 4, 4);
    check(!snapshot.Undo(edit, [] { return true; }) && read() == L"前缀再见", "refuse changed document");
    seed();
    snapshot.Arm(edit, L"你好");
    snapshot.Capture(edit);
    SendMessageW(edit, EM_SETSEL, 2, 2);
    check(!snapshot.Undo(edit, [] { return true; }) && read() == L"前缀你好", "refuse moved caret");
    seed();
    snapshot.Arm(edit, L"你好");
    snapshot.Capture(edit);
    check(!snapshot.Undo(nullptr, [] { return true; }), "refuse another input");
    seed();
    snapshot.Arm(edit, L"你好");
    snapshot.Capture(edit);
    int calls = 0;
    check(!snapshot.Undo(edit, [&] { return ++calls == 1; }) && read() == L"前缀你好", "refuse reentrant invalidation");
    seed();
    snapshot.Arm(edit, L"你好");
    snapshot.Capture(edit);
    calls = 0;
    check(!snapshot.Undo(edit,
                         [&] {
                             if (++calls == 2)
                                 SendMessageW(edit, EM_SETSEL, 0, 2);
                             return true;
                         }) &&
              read() == L"前缀你好",
          "refuse reentrant selection move");
    seed();
    SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    snapshot.Arm(edit, L"你好");
    check(!snapshot.Capture(edit), "refuse read-only control");
    SendMessageW(edit, EM_SETREADONLY, FALSE, 0);
    seed();
    snapshot.Arm(edit, L"再见");
    check(!snapshot.Capture(edit), "refuse unrelated suffix");
    DestroyWindow(edit);
    if (!failures)
        std::cout << "Native EDIT undo and invalidation checks passed\n";
    return failures ? 1 : 0;
}
