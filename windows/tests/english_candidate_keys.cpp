#ifdef NDEBUG
#undef NDEBUG
#endif
#include "Key/KeyEventSinkInternal.h"
#include <cassert>
using key_event_sink_detail::ClassifyEnglishCandidateKey;
int main()
{
    _KEYSTROKE_STATE state{};
    assert(ClassifyEnglishCandidateKey('H', L'h', false, false, false, &state));
    assert(state.Category == CATEGORY_COMPOSING && state.Function == FUNCTION_INPUT);
    // Keep extending the same preedit after suggestions become visible.
    for (const wchar_t ch : L"ello")
    {
        if (!ch)
            break;
        assert(ClassifyEnglishCandidateKey(static_cast<UINT>(ch - L'a' + L'A'), ch, true, true, false, &state));
        assert(state.Category == CATEGORY_COMPOSING && state.Function == FUNCTION_INPUT);
    }
    assert(!ClassifyEnglishCandidateKey('H', L'H', false, false, true, &state));
    assert(ClassifyEnglishCandidateKey('H', L'H', true, false, true, &state));
    assert(!ClassifyEnglishCandidateKey(VK_BACK, 0, false, false, false, &state));
    assert(ClassifyEnglishCandidateKey(VK_BACK, 0, true, false, false, &state));
    assert(state.Function == FUNCTION_BACKSPACE);
    assert(ClassifyEnglishCandidateKey(VK_SPACE, L' ', true, false, false, &state));
    assert(state.Function == FUNCTION_CONVERT);
    assert(ClassifyEnglishCandidateKey(VK_RETURN, L'\r', true, false, false, &state));
    assert(state.Function == FUNCTION_FINALIZE_CANDIDATELISTForVKReturn);
    assert(ClassifyEnglishCandidateKey('2', L'2', true, true, false, &state));
    assert(state.Function == FUNCTION_SELECT_BY_NUMBER);
    assert(!ClassifyEnglishCandidateKey(VK_F5, 0, true, false, false, &state));
    return 0;
}
