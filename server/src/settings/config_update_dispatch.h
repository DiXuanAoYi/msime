#pragma once
#include <boost/json.hpp>
#include <optional>

namespace SettingsConfig
{
// Unknown paths return false; recognized settings preserve the native setters' validation.
bool ApplyConfigUpdate(const boost::json::object &data);
// nullopt: another setting; false: recognized but rejected/failed; true: persisted.
std::optional<bool> ApplyCandidateAnnotationUpdate(const boost::json::object &data);
} // namespace SettingsConfig
