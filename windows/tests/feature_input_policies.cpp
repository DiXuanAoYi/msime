#include "../../engine/contracts/trilingual_input.h"
#include "../../engine/contracts/field_privacy.h"
#include "../../engine/contracts/ai_input_shortcuts.h"
#include "../../server/src/ipc/input_key_policy.h"
#include "../src/Key/VimMode.h"
#include "../src/Key/DeferredKeyFailurePolicy.h"
#include <cassert>

int main()
{
    using namespace FanyImeTrilingualInput;
    static_assert((CycleRequestMask & FanyImeFieldPrivacy::Mask) == 0);
    assert(Next(true, false) == Mode::Japanese);
    assert(Next(true, true) == Mode::English);
    assert(Next(false, true) == Mode::Chinese);
    assert(Destination(true, false, false, false) == Mode::English);
    assert(Destination(false, false, false, true) == Mode::Japanese);
    auto flags = EncodeCycleRequest(true, false);
    assert(IsCycleRequest(0x10u, flags));
    assert(CycleRequestImeEnabled(flags) && !CycleRequestJapanese(flags));
    assert(!IsCycleRequest(0x10u, 0));
    assert(!IsCycleRequest(0x10u, flags | 2));
    assert(!IsCycleRequest('A', flags));
    Mode mode;
    std::wstring text;
    assert(ParsePayload(BuildPayload(Mode::Japanese, L"你好raw"), mode, text));
    assert(mode == Mode::Japanese && text == L"你好raw");
    assert(!ParsePayload(L"3\ttext", mode, text));
    assert(!ParsePayload(L"1text", mode, text));
    assert(ResolveDeferredKeyFailure(DeferredKeyFailureReason::HostEditRejected, false, true) ==
           DeferredKeyFailureKind::Transport);
    assert(ResolveDeferredKeyFailure(DeferredKeyFailureReason::ProjectionInvalidated, false, true) ==
           DeferredKeyFailureKind::Resync);
    assert(VimMode::ShouldSwitchToEnglish(0x1b, false, false, 0, false, false, false));
    assert(!VimMode::ShouldSwitchToEnglish(0x1b, true, false, 0, false, false, false));
    assert(!VimMode::ShouldSwitchToEnglish(0x1b, false, false, 0, false, true, false));
    assert(!VimMode::ShouldSwitchToEnglish(0x1b, false, false, 0, false, false, true));
    assert(!VimMode::ShouldSwitchToEnglish(0x1b, false, true, 0, false, false, false));
    assert(FanyImeIpc::IsPinyinCommitKey(0x0d, 1));
    assert(!FanyImeIpc::IsPinyinCommitKey(0x0d, 3));
    assert(FanyImeIpc::EnteredPinyinText("nihk", "ni'hao", true) == "nihao");
    assert(FanyImeIpc::EnteredPinyinText("Rnihk", "ni'hao", false) == "Rnihk");
    assert(FanyImeIpc::EnteredPinyinText("NIhk", "", true) == "NIhk");
    using FanyImeAiInput::Task;
    assert(FanyImeAiInput::Shortcut('Q', 6, true, false, true) == Task::Question);
    assert(FanyImeAiInput::Shortcut(0x0d, 6, true, true, false) == Task::Completion);
    assert(FanyImeAiInput::Shortcut(0x20, 6, true, true, true) == Task::None); // configured language hotkey
    assert(FanyImeAiInput::Shortcut('Q', 6, false, true, true) == Task::None); // application shortcut
    assert(FanyImeAiInput::Shortcut('Q', 6, true, true, false) == Task::None); // default-off question
    assert(FanyImeAiInput::Shortcut(0x0d, 1, true, true, true) == Task::None); // Shift+Enter pinyin
}
