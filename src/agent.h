#pragma once
#include <windows.h>

// The agent runs in each user session (rundll32 G25FF.dll,Agent, started at logon). Whenever a HID device
// arrives it switches G25s in compatibility mode to native mode, and applies the rotation range and autocenter
// settings to G25s in native mode. One per session.
constexpr wchar_t AgentMutexName[] = L"Local\\G25FF.Agent";
constexpr wchar_t AgentExitEventName[] = L"Local\\G25FF.AgentExit";

void RunAgent();
