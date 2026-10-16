#pragma once
// The global-hotkey actions: screenshot, OCR, screenshot translation, selection translation, live
// translation, reply, rewrite and the clipboard panel. Everything runs on the quick thread; slow work
// (OCR, network, reading other apps) runs on short-lived workers that hand the result back with Post.

#include <Windows.h>
#include <nlohmann/json.hpp>
#include <string>

namespace Quick::Actions
{
// Called by the quick thread.
void OnStart();
void OnStop();
void OnHotkey(int id);
void OnTimer(UINT_PTR id);

// The Server owns the hotkeys. The settings process must not register them, or both would fight over the
// same keys; it only uses Run for the "try it" buttons. Call before Quick::Start().
void EnableHotkeys(bool enabled);

// Runs an action by name (shot, ocr, translate, selection, live, reply, rewrite, clip) as if its hotkey
// were pressed. Returns false for an unknown name. Safe from any thread; starts the quick thread if needed.
bool Run(const std::string &id);

// {"hotkeys": {action: {combo, ok, error}}} as last written by the Server, for the settings page.
nlohmann::json HotkeyStatus();
} // namespace Quick::Actions
