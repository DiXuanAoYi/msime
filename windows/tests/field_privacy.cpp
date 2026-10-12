#include "../../engine/contracts/field_privacy.h"
#include "../../engine/contracts/ipc_negotiation.h"
#include "../../server/src/ipc/input_privacy_policy.h"
#include "../../server/src/ipc/ai_context_policy.h"
#include <cassert>

int main()
{
    using namespace FanyImeFieldPrivacy;
    static_assert((Mask & 0xfc000000u) == 0);
    for (State state : {State::Unknown, State::Regular, State::Sensitive})
    {
        assert(Decode(Encode(state), true) == state);
        assert(Decode(Encode(state), false) == State::Unknown);
    }
    assert(Decode(SensitiveFlag, true) == State::Sensitive);
    assert(!AllowsAutomaticNetwork(State::Unknown));
    assert(!AllowsAutomaticNetwork(State::Sensitive));
    assert(AllowsAutomaticNetwork(State::Regular));
    assert(AllowsLocalHistory(State::Unknown));
    assert(!AllowsLocalHistory(State::Sensitive));
    FanyImeIpc::input_privacy = {42, 7, State::Unknown};
    assert(!FanyImeIpc::AutomaticInputNetworkAllowed());
    assert(FanyImeIpc::AutomaticInputAiAllowed(true));
    assert(!FanyImeIpc::AutomaticInputAiAllowed(false));
    FanyImeIpc::input_privacy.state = State::Regular;
    assert(FanyImeIpc::AutomaticInputNetworkAllowed());
    FanyImeIpc::input_privacy.epoch = 0;
    assert(!FanyImeIpc::AutomaticInputNetworkAllowed());
    FanyImeIpc::input_privacy = {42, 7, State::Sensitive};
    assert(!FanyImeIpc::InputLearningAllowed());
    assert(!FanyImeIpc::AutomaticInputAiAllowed(true));
    assert(!FanyImeIpc::AutomaticInputAiAllowed(false));

    auto hello = FanyImeProtocol::Hello(42, 99, FanyImeProtocol::Capabilities | FanyImeProtocol::FieldPrivacy);
    const auto old = FanyImeProtocol::Negotiate(hello);
    assert(old.accepted && (old.capabilities & FanyImeProtocol::FieldPrivacy) == 0);
    const auto modern =
        FanyImeProtocol::Negotiate(hello, FanyImeProtocol::Capabilities | FanyImeProtocol::FieldPrivacy);
    assert(modern.accepted && (modern.capabilities & FanyImeProtocol::FieldPrivacy));

    const std::string original = "你好 plainword Abc12345 world";
    assert(FanyImeIpc::StripCredentialLikeText(original) == "你好 plainword  world");
    assert(original == "你好 plainword Abc12345 world");
    assert(!FanyImeIpc::IsCredentialLikeAsciiRun("Abc1234"));
    assert(FanyImeIpc::IsCredentialLikeAsciiRun("Abc12345"));
    assert(!FanyImeIpc::IsCredentialLikeAsciiRun("12345678")); // PIN protection belongs to the field gate.
}
