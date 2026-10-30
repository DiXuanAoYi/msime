#pragma once

#include <boost/json.hpp>
#include <string>

namespace SettingsDictionary
{
std::string CandidateReading(const std::string &word);
boost::json::object HandleRequest(const boost::json::object &request);
} // namespace SettingsDictionary
