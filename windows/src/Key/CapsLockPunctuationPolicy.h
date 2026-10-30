#pragma once

inline bool FollowedPunctuationOpen(bool imeOpen, bool capsLockEnabled)
{
    return imeOpen && !capsLockEnabled;
}

// Duplicate TSF/Server reports of the same edge must not undo Ctrl+. overrides.
inline bool ShouldResyncPunctuationForCapsLock(bool known, bool lastApplied, bool capsLockEnabled)
{
    return !known || lastApplied != capsLockEnabled;
}
