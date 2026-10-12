#include "Private.h"
#include "MetasequoiaIME.h"
#include "EditSession.h"
#include <wrl/client.h>
#include <array>

namespace
{
using Microsoft::WRL::ComPtr;
bool ReadComplete(ITfRange *range, TfEditCookie ec, std::wstring &text)
{
    ComPtr<ITfRange> cursor;
    if (FAILED(range->Clone(&cursor)))
        return false;
    text.clear();
    for (;;)
    {
        BOOL empty = FALSE;
        if (FAILED(cursor->IsEmpty(ec, &empty)))
            return false;
        if (empty)
            return true;
        std::array<WCHAR, 128> buffer{};
        ULONG count = 0;
        if (FAILED(cursor->GetText(ec, TF_TF_MOVESTART, buffer.data(), static_cast<ULONG>(buffer.size()), &count)) ||
            count == 0 || text.size() + count > 4096)
            return false;
        text.append(buffer.data(), count);
    }
}
class UndoSession final : public CEditSessionBase
{
  public:
    UndoSession(CMetasequoiaIME *ime, ITfContext *context, uint64_t token, uint64_t serial)
        : CEditSessionBase(ime, context), token_(token), serial_(serial)
    {
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        return _pTextService->_UndoLastCommit(ec, _pContext, token_, serial_);
    }

  private:
    uint64_t token_, serial_;
};
} // namespace
void CMetasequoiaIME::_ClearLastCommit()
{
    _nativeUndo.Clear();
    auto range = _undoRange;
    auto context = _undoContext;
    _undoRange = nullptr;
    _undoContext = nullptr;
    _undoText.clear();
    _undoFocus = 0;
    ++_undoSerial;
    if (range)
        range->Release();
    if (context)
        context->Release();
}
void CMetasequoiaIME::_RememberLastCommit(TfEditCookie ec, ITfComposition *composition)
{
    _ClearLastCommit();
    if (!composition)
        return;
    ComPtr<ITfRange> original, range;
    ComPtr<ITfContext> context;
    if (FAILED(composition->GetRange(&original)) || !original || FAILED(original->Clone(&range)) ||
        FAILED(range->GetContext(&context)))
        return;
    std::wstring text;
    if (!ReadComplete(range.Get(), ec, text) || text.empty())
        return;
    const auto token = _CaptureFocusSessionToken();
    if (!token || !_IsFocusSessionCurrent(token, context.Get()))
        return;
    _undoText = std::move(text);
    _undoRange = range.Detach();
    _undoContext = context.Detach();
    _undoFocus = token;
    _nativeUndo.Arm(GetFocus(), _undoText);
    PostMessageW(_msgWndHandle, NativeCommitUndo::CaptureMessage, static_cast<WPARAM>(_undoSerial & 0xffffffff),
                 static_cast<LPARAM>(_undoSerial >> 32));
}
void CMetasequoiaIME::_CaptureNativeLastCommit(uint64_t serial)
{
    if (serial != _undoSerial || !_undoRange || _IsComposing() || _nativeUndo.Ready())
        return;
    auto snapshot = _nativeUndo;
    if (snapshot.Capture(GetFocus()) && serial == _undoSerial)
        _nativeUndo = std::move(snapshot);
}
bool CMetasequoiaIME::_IsUndoCommitKey(ITfContext *context, WPARAM key)
{
    // A legacy store may pump the posted capture while its composition is
    // still active. The first shortcut test runs after that commit completes.
    if (key == VK_BACK && _undoRange && _undoContext == context && !_IsComposing() && !_nativeUndo.Ready())
        _CaptureNativeLastCommit(_undoSerial);
    return key == VK_BACK && !_IsComposing() && _undoRange && _undoContext == context &&
           (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_SHIFT) & 0x8000) &&
           !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000) &&
           _IsFocusSessionCurrent(_undoFocus, context) && !_IsKeyboardDisabled();
}
void CMetasequoiaIME::_RequestUndoCommit(ITfContext *context)
{
    if (!context || !_undoRange || _IsComposing())
        return;
    if (_nativeUndo.Ready())
    {
        const auto focus = _undoFocus;
        if (_undoContext != context || !_IsFocusSessionCurrent(focus, context))
            return;
        auto snapshot = std::move(_nativeUndo);
        _ClearLastCommit();
        const auto serial = _undoSerial;
        const auto window = GetFocus();
        snapshot.Undo(window, [&] {
            return GetFocus() == window && _undoSerial == serial && !_IsComposing() &&
                   _IsFocusSessionCurrent(focus, context);
        });
        return;
    }
    auto *session = new (std::nothrow) UndoSession(this, context, _undoFocus, _undoSerial);
    if (!session)
        return;
    HRESULT result = E_FAIL;
    auto hr = context->RequestEditSession(_tfClientId, session, TF_ES_ASYNCDONTCARE | TF_ES_READWRITE, &result);
    session->Release();
    if (FAILED(hr) || FAILED(result))
        _ClearLastCommit();
}
HRESULT CMetasequoiaIME::_UndoLastCommit(TfEditCookie ec, ITfContext *context, uint64_t token, uint64_t serial)
{
    if (!_undoRange || _undoContext != context || token != _undoFocus || serial != _undoSerial || _IsComposing() ||
        !_IsFocusSessionCurrent(token, context))
        return S_FALSE;
    // Retain exact objects and consume the record before COM calls can re-enter.
    ComPtr<ITfRange> range = _undoRange;
    const auto expected = _undoText;
    _ClearLastCommit();
    const auto consumedSerial = _undoSerial;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (FAILED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) || fetched != 1)
        return S_FALSE;
    ComPtr<ITfRange> caret;
    caret.Attach(selection.range);
    BOOL empty = FALSE;
    LONG comparison = 1;
    if (FAILED(caret->IsEmpty(ec, &empty)) || !empty ||
        FAILED(caret->CompareStart(ec, range.Get(), TF_ANCHOR_END, &comparison)) || comparison != 0)
        return S_FALSE;
    std::wstring actual;
    if (!ReadComplete(range.Get(), ec, actual) || actual != expected)
        return S_FALSE;
    if (_undoSerial != consumedSerial || _IsComposing() || !_IsFocusSessionCurrent(token, context))
        return S_FALSE;
    const HRESULT hr = range->SetText(ec, 0, L"", 0);
    if (FAILED(hr))
        return hr;
    if (FAILED(range->Collapse(ec, TF_ANCHOR_START)))
        return S_OK;
    TF_SELECTION next{};
    next.range = range.Get();
    next.style.ase = TF_AE_NONE;
    next.style.fInterimChar = FALSE;
    context->SetSelection(ec, 1, &next);
    return S_OK;
}
