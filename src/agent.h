#pragma once
#include <windows.h>

// The agent runs in each user session (G25FFAgent.exe, started at logon). Whenever a HID device arrives it
// switches wheels in compatibility mode to native mode, and applies the rotation range and autocenter settings to
// wheels in native mode. One per session.
constexpr wchar_t AgentMutexName[] = L"Local\\G25FF.Agent";
constexpr wchar_t AgentExitEventName[] = L"Local\\G25FF.AgentExit";

void RunAgent();
