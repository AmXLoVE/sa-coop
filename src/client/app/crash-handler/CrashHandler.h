#pragma once
#include <string>

namespace coop {

// Writes coop\crash.log (exception, module+offset, registers, probable call stack) for fatal
// exceptions. First-chance access violations are logged too (limited), because mods and the game
// sometimes swallow them and the real crash happens later.
void InstallCrashHandler(const std::wstring& crashLogPath);

// Hang detector: the main thread calls Heartbeat() regularly (frame hooks, skeleton events);
// if no heartbeat arrives for `seconds`, the main thread stack is dumped into crash.log once.
void StartHangDetector(unsigned seconds);
void Heartbeat();

}  // namespace coop
