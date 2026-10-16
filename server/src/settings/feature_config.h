#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <toml++/toml.h>

// String-valued config sections for the quick actions: [hotkeys], [reply] and [clipboard].
// Same storage rules as [vision]: every value is a TOML basic string, every key has a default,
// and Set validates before anything is written.
namespace FeatureConfig
{
using Values = std::map<std::string, std::string>;

Values Defaults(const std::string &section);
Values Get(const std::string &section);
std::string Get(const std::string &section, const std::string &key);
bool GetBool(const std::string &section, const std::string &key);
int GetInt(const std::string &section, const std::string &key);
void Load(const toml::table &table);
bool Set(const std::string &section, const std::string &key, const std::string &value);
// Bumped by every Load and Set. Long-lived workers compare it to notice that a section changed.
uint64_t Revision();
// Names of the actions that own a global hotkey, in settings display order.
const char *const *HotkeyActions(size_t &count);
} // namespace FeatureConfig
