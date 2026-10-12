#pragma once
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include "engine/core/word_item.h"
namespace PersonalLibrary
{
nlohmann::json Handle(const nlohmann::json &request, HWND owner);
std::vector<std::string> AppWords(const std::string &application, const std::string &code);
std::string ForegroundApplication();
std::string ProcessApplication(uint32_t process_id);
// The serialized IPC worker binds the authenticated TSF origin before applying a key.
void BindInputApplication(std::string application);
const std::string &InputApplication();
struct ApplicationPolicy
{
    bool isolated = false;
    bool learn = true;
    bool frequency = true;
};
ApplicationPolicy Policy(const std::string &application);
// Isolation stays fail-closed on a read/write error: callers must not then learn globally.
void RecordSelection(const std::string &application, const std::string &code, const std::string &word,
                     const std::string &language, bool insert = false);
void ApplyApplicationCandidates(const std::string &application, const std::string &code,
                                std::vector<WordItem> &candidates, bool english = false,
                                const std::string &canonical_code = {});
void SetCandidatePreference(const std::string &application, const std::string &code, const std::string &word,
                            bool english, const std::string &action, int position = 0);
} // namespace PersonalLibrary
