#pragma once

#include "engine/contracts/field_privacy.h"

namespace FanyImeIpc
{
struct InputPrivacyBinding
{
    std::uint64_t client = 0;
    std::uint64_t epoch = 0;
    FanyImeFieldPrivacy::State state = FanyImeFieldPrivacy::State::Unknown;
};

// Authoritative worker-thread state, shared by its session adapter and writers.
// It never carries document text and is not read from background AI callbacks.
inline thread_local InputPrivacyBinding input_privacy;

inline bool InputLearningAllowed()
{
    return FanyImeFieldPrivacy::AllowsLocalHistory(input_privacy.state);
}

inline bool AutomaticInputNetworkAllowed()
{
    return input_privacy.client != 0 && input_privacy.epoch != 0 &&
           FanyImeFieldPrivacy::AllowsAutomaticNetwork(input_privacy.state);
}

inline bool AutomaticInputAiAllowed(bool verified_loopback_local)
{
    return input_privacy.client != 0 && input_privacy.epoch != 0 &&
           input_privacy.state != FanyImeFieldPrivacy::State::Sensitive &&
           (verified_loopback_local || FanyImeFieldPrivacy::AllowsAutomaticNetwork(input_privacy.state));
}
} // namespace FanyImeIpc
