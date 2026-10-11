#pragma once

#include <string>
#include <ctime>
#include <vector>

namespace TencentTmt
{
struct Credentials
{
    std::string secret_id;
    std::string secret_key;
    std::string region = "ap-guangzhou";
};

std::string TtsAuthorization(const Credentials &credentials, const std::string &payload, time_t timestamp);

// ASR uses the same TC3 primitive, with its own service and signed action.
std::string AsrAuthorization(const Credentials &credentials, const std::string &payload, time_t timestamp);
std::string OcrAuthorization(const Credentials &credentials, const std::string &payload, time_t timestamp,
                             const std::string &action);
// Returns one translation per source string. Empty entries mean that item failed.
std::vector<std::string> TextTranslateBatch(const Credentials &credentials, const std::vector<std::string> &texts,
                                            const std::string &source, const std::string &target);
} // namespace TencentTmt
