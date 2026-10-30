#pragma once
#include "config/ime_config.h"
#include <msime/voice/provider_protocol.h>
#include <string>
#include <vector>

namespace VoiceInput
{
metasequoia::voice::MultipartRequest BuildBatchTranscription(const std::vector<float> &samples,
                                                             const VoiceInputConfig &config);
struct AsrHttpRequest
{
    std::string endpoint;
    std::string content_type;
    std::string body;
    std::vector<std::string> headers;
};
AsrHttpRequest BuildAsrHttpRequest(const std::vector<float> &samples, const VoiceInputConfig &config,
                                   int64_t timestamp);
std::string ParseAsrHttpResponse(const std::string &body, const std::string &provider);
std::string BuildBatchPolish(const std::string &text, const VoiceInputConfig &config);
} // namespace VoiceInput
