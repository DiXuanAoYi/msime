#pragma once

#include "config/ime_config.h"

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace VoiceInput
{
struct PolishPromptPreset
{
    std::string_view id;
    std::string_view name;
    std::string_view prompt;
};

// Doubao console generations. The new console issues a single API Key sent as X-Api-Key;
// the legacy console issues App ID + Access Token sent as X-Api-App-Key + X-Api-Access-Key.
// Both are accepted by the same endpoints, so this only selects which headers to send.
inline constexpr std::string_view kDoubaoAuthApiKey = "api_key";
inline constexpr std::string_view kDoubaoAuthLegacy = "legacy";

// Streaming endpoints. bigmodel_nostream streams audio up but returns whole-sentence
// results; Volcengine documents it as more accurate and recommends it for IME input.
inline constexpr std::string_view kDoubaoEndpointNostream =
    "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_nostream";
inline constexpr std::string_view kDoubaoEndpointAsync = "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async";

bool IsDoubaoAsrProvider(std::string_view provider);
std::string NormalizeProviderId(std::string_view provider);
// Resolves an explicit doubao_auth_mode, falling back to inferring the legacy console from a
// usable App ID when the key is absent (configs written before the setting existed).
std::string NormalizeDoubaoAuthMode(std::string_view mode, std::string_view app_key);
bool UsesDoubaoLegacyAuth(const VoiceInputConfig &config);
bool IsPlaceholderToken(std::string_view token);
std::string UsableToken(std::string_view token);
std::string AsrTokenSlotKey(std::string_view provider);
std::string PolishTokenSlotKey(std::string_view provider);
std::string ResolveAsrToken(const VoiceInputConfig &config);
std::string ResolvePolishToken(const VoiceInputConfig &config);
const std::vector<std::string_view> &AsrProviders();
const std::vector<std::string_view> &PolishProviders();
std::string DefaultAsrEndpoint(std::string_view provider);
std::string DefaultAsrModel(std::string_view provider);
std::string DefaultPolishEndpoint(std::string_view provider);
std::string DefaultPolishModel(std::string_view provider);
std::string ResolveAsrModel(const VoiceInputConfig &config);
std::string ResolvePolishModel(const VoiceInputConfig &config);
const std::vector<PolishPromptPreset> &BuiltinPolishPromptPresets();
bool IsPolishPromptId(std::string_view id);
std::string ResolvePolishSystemPrompt(const VoiceInputConfig &config);
std::string WrapAsrUserMessage(const std::string &asr_text);

// One chat-completions connection. Voice polish, reply generation and screen translation all draw
// their credential from the same per-provider token slots, so a key is entered once.
struct TextModelConnection
{
    std::string provider, endpoint, model, token;
    bool ready() const
    {
        return !endpoint.empty() && !model.empty() && !token.empty();
    }
};
// An empty provider_override means "the provider selected for voice polish".
TextModelConnection ResolveTextModel(const VoiceInputConfig &config, std::string_view provider_override);
// Adds the provider-specific switches that turn thinking off, so replies stay fast.
void ApplyTextModelDefaults(const TextModelConnection &connection, nlohmann::json &payload);
} // namespace VoiceInput
