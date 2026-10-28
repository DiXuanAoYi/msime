#pragma once
#include <windows.h>

// Local, read-only status feed for the desktop companion (a separate process). It only carries geometry and
// booleans -- never text, key codes or candidates -- over the per-user named pipe \\.\pipe\MetasequoiaImeCompanion,
// one JSON object per line. Publishing never blocks the caller: it updates a snapshot and wakes the writer thread.
namespace Companion
{
void Start();
void Stop();
// The candidate window became visible at `bounds` (screen pixels), or hidden.
void SetCandidate(bool visible, const RECT &bounds);
// Voice input started or stopped recording.
void SetVoiceRecording(bool recording);
} // namespace Companion
