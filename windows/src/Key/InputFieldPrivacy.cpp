#include "Private.h"
#include "InputFieldPrivacy.h"
#include "EditSession.h"
#include "MetasequoiaIME.h"
#include <InputScope.h>
#include <new>

namespace
{
constexpr GUID kGuidPropInputScope = {0x1713dd5a, 0x68e7, 0x4a5b, {0x9a, 0xf6, 0x59, 0x2a, 0x59, 0x5c, 0x77, 0x8d}};

bool IsSensitiveScope(InputScope scope)
{
    return scope == IS_PASSWORD || scope == IS_NUMERIC_PASSWORD || scope == IS_NUMERIC_PIN ||
           scope == IS_ALPHANUMERIC_PIN || scope == IS_ALPHANUMERIC_PIN_SET;
}

FanyImeFieldPrivacy::State ReadInputScope(ITfContext *context, TfEditCookie ec)
{
    using FanyImeFieldPrivacy::State;
    State result = State::Unknown;
    ITfProperty *property = nullptr;
    if (FAILED(context->GetProperty(kGuidPropInputScope, &property)) || !property)
        return result;
    TF_SELECTION selection{};
    ULONG fetched = 0;
    if (SUCCEEDED(context->GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)) && fetched == 1 &&
        selection.range)
    {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(property->GetValue(ec, selection.range, &value)) && value.vt == VT_UNKNOWN && value.punkVal)
        {
            ITfInputScope *inputScope = nullptr;
            if (SUCCEEDED(
                    value.punkVal->QueryInterface(__uuidof(ITfInputScope), reinterpret_cast<void **>(&inputScope))) &&
                inputScope)
            {
                InputScope *scopes = nullptr;
                UINT count = 0;
                if (SUCCEEDED(inputScope->GetInputScopes(&scopes, &count)) && scopes)
                {
                    bool ordinary = false;
                    for (UINT i = 0; i < count; ++i)
                    {
                        if (IsSensitiveScope(scopes[i]))
                        {
                            result = State::Sensitive;
                            break;
                        }
                        // Missing/default/grammar scopes are not reliable declarations.
                        ordinary = ordinary || (scopes[i] > IS_DEFAULT && scopes[i] <= IS_CHAT_WITHOUT_EMOJI);
                    }
                    if (result != State::Sensitive && ordinary)
                        result = State::Regular;
                    CoTaskMemFree(scopes);
                }
                inputScope->Release();
            }
        }
        VariantClear(&value);
    }
    if (selection.range)
        selection.range->Release();
    property->Release();
    return result;
}

class CFieldPrivacyEditSession : public CEditSessionBase
{
  public:
    CFieldPrivacyEditSession(CMetasequoiaIME *service, ITfContext *context, FanyImeFieldPrivacy::State *result)
        : CEditSessionBase(service, context), result_(result)
    {
    }
    STDMETHODIMP DoEditSession(TfEditCookie ec) override
    {
        *result_ = ReadInputScope(_pContext, ec);
        return S_OK;
    }

  private:
    FanyImeFieldPrivacy::State *result_;
};
} // namespace

FanyImeFieldPrivacy::State ResolveInputFieldPrivacy(CMetasequoiaIME *service, ITfContext *context, TfClientId clientId)
{
    using FanyImeFieldPrivacy::State;
    State result = State::Unknown;
    if (!context)
        return result;
    auto *session = new (std::nothrow) CFieldPrivacyEditSession(service, context, &result);
    if (!session)
        return result;
    HRESULT sessionResult = E_FAIL;
    const HRESULT hr = context->RequestEditSession(clientId, session, TF_ES_SYNC | TF_ES_READ, &sessionResult);
    session->Release();
    return hr == S_OK && sessionResult == S_OK ? result : State::Unknown;
}
