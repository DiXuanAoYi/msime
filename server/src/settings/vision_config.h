#pragma once
#include <map>
#include <string>
#include <toml++/toml.h>
namespace VisionConfig
{
using Config = std::map<std::string, std::string>;
Config Defaults();
Config Get();
void Load(const toml::table &table);
bool Set(const std::string &key, const std::string &value);
} // namespace VisionConfig
