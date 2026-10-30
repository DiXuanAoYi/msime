#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

// The reply assistant's personal data: which tones appear in the panel, the user's speaking habits,
// and per-contact memory. One DPAPI-encrypted document per Windows account.
namespace ReplyStudio
{
std::filesystem::path ProfilePath();

// Tones in panel order, built-in ones merged with the stored order and switches. Every entry has
// id, name, desc, example, on, custom and (custom only) prompt.
nlohmann::json LoadTones();
nlohmann::json LoadPersona();
// "我的说话习惯" as one sentence for the model; empty when nothing is set.
std::string PersonaPrompt(const nlohmann::json &persona, const std::string &contactAddress);

// Looks a contact up by stable key (platform|title). Returns null when unknown.
nlohmann::json FindContact(const std::string &key);
// Creates or updates the contact for a captured chat window and merges new messages, then prunes by the
// contact's retention. Returns the stored contact without its message list.
nlohmann::json ObserveChat(const nlohmann::json &parsedWindow, int64_t now);
// Messages kept for a contact, newest last, at most `limit` of them.
nlohmann::json RecentMessages(const std::string &key, size_t limit);
// Text for the model: relationship, address form, notes and summary.
std::string RelationshipPrompt(const nlohmann::json &contact);
std::string ContactKey(const std::string &platform, const std::string &title);

// Entry point for the settings page (replyProfileRequest).
nlohmann::json HandleProfileRequest(const nlohmann::json &request);
} // namespace ReplyStudio
