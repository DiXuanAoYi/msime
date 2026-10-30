#pragma once
#include "engine/contracts/webview/validator.h"
#include <boost/json.hpp>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
namespace SettingsWebView
{
inline std::string SerializeReply(nlohmann::json payload)
{
    payload["protocolVersion"] = metasequoia::webview::Version;
    auto value = boost::json::parse(payload.dump());
    if (!metasequoia::webview::Validate(value, "server"))
        throw std::runtime_error("Invalid settings reply");
    return boost::json::serialize(value);
}
} // namespace SettingsWebView
