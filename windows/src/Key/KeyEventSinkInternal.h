#pragma once

// Declarations shared by KeyEventSink*.cpp: helpers that lived in KeyEventSink.cpp's anonymous
// namespace before the split and are now used by more than one of those files.
// Include only from windows/src/Key/KeyEventSink*.cpp.

#include "Private.h"
#include "MetasequoiaIMEBaseStructure.h"

// 0xF003, 0xF004 are the keys that the touch keyboard sends for next/previous
#define THIRDPARTY_NEXTPAGE static_cast<WORD>(0xF003)
#define THIRDPARTY_PREVPAGE static_cast<WORD>(0xF004)

namespace key_event_sink_detail
{
KEYSTROKE_FUNCTION SegmentEditFunction(UINT code, UINT modifiers);
UINT CaptureIpcModifiers();
bool IsEnglishInputModeToggle(UINT code, UINT modifiers);
bool IsTranslationCommitShortcut(UINT code, UINT modifiers);
bool IsCharacterSetInputModeToggle(UINT code, UINT modifiers);
// Shared by immediate and deferred classification: English candidates use the
// same edit-session dispatch and never replay letters into the application.
inline bool ClassifyEnglishCandidateKey(UINT code, WCHAR ch, bool active, bool /*candidate*/, bool caps,
                                        _KEYSTROKE_STATE *state)
{
    KEYSTROKE_CATEGORY category = CATEGORY_COMPOSING;
    KEYSTROKE_FUNCTION function = FUNCTION_NONE;
    if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z'))
    {
        if (caps && !active)
            return false;
        // Visible suggestions do not end an English word. Finalize-and-input
        // commits the previous preedit and starts over for every next letter.
        function = FUNCTION_INPUT;
    }
    else if (active)
    {
        switch (code)
        {
        case VK_BACK:
            function = FUNCTION_BACKSPACE;
            break;
        case VK_DELETE:
            function = FUNCTION_DELETE;
            break;
        case VK_LEFT:
            function = FUNCTION_MOVE_LEFT;
            break;
        case VK_RIGHT:
            function = FUNCTION_MOVE_RIGHT;
            break;
        case VK_ESCAPE:
            category = CATEGORY_CANDIDATE;
            function = FUNCTION_CANCEL;
            break;
        case VK_RETURN:
            category = CATEGORY_CANDIDATE;
            function = FUNCTION_FINALIZE_CANDIDATELISTForVKReturn;
            break;
        case VK_SPACE:
            category = CATEGORY_CANDIDATE;
            function = FUNCTION_CONVERT;
            break;
        case VK_TAB:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_UP:
        case VK_DOWN:
            category = CATEGORY_CANDIDATE;
            function = FUNCTION_SERVER_CANDIDATE_KEY;
            break;
        default:
            if (code >= L'1' && code <= L'9')
            {
                category = CATEGORY_CANDIDATE;
                function = FUNCTION_SELECT_BY_NUMBER;
            }
            else if (ch >= 0x20 && ch < 0x7f)
                function = FUNCTION_PUNCTUATION;
        }
    }
    if (function == FUNCTION_NONE)
        return false;
    if (state)
    {
        state->Category = category;
        state->Function = function;
    }
    return true;
}
void PostOwnerMessageWithSyncFallback(HWND window, UINT message, WPARAM wParam = 0, LPARAM lParam = 0);
bool IsShiftVk(UINT code);
} // namespace key_event_sink_detail

// Because the code mostly works with VKeys, here map a WCHAR back to a VKKey for certain
// vkeys that the IME handles specially
__inline UINT VKeyFromVKPacketAndWchar(UINT vk, WCHAR wch)
{
    UINT vkRet = vk;
    if (LOWORD(vk) == VK_PACKET)
    {
        if (wch == L' ')
        {
            vkRet = VK_SPACE;
        }
        else if ((wch >= L'0') && (wch <= L'9'))
        {
            vkRet = static_cast<UINT>(wch);
        }
        else if ((wch >= L'a') && (wch <= L'z'))
        {
            vkRet = (UINT)(L'A') + ((UINT)(L'z') - static_cast<UINT>(wch));
        }
        else if ((wch >= L'A') && (wch <= L'Z'))
        {
            vkRet = static_cast<UINT>(wch);
        }
        else if (wch == THIRDPARTY_NEXTPAGE)
        {
            vkRet = VK_NEXT;
        }
        else if (wch == THIRDPARTY_PREVPAGE)
        {
            vkRet = VK_PRIOR;
        }
    }
    return vkRet;
}
