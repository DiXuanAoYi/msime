#pragma once
#include <Windows.h>

namespace SettingsEnvironment
{
// Capture on the Settings main thread before starting its worker. Probe only
// existence/length; paths and other environment variables never enter the snapshot.
inline bool HasTemporaryConfiguration()
{
    // A zero-sized buffer reports required capacity including the terminator.
    // An existing empty value therefore reports 1; only values above 1 are nonempty.
    return GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0) > 1 ||
           GetEnvironmentVariableW(L"METASEQUOIA_IME_CONFIG_DIR", nullptr, 0) > 1;
}
} // namespace SettingsEnvironment
