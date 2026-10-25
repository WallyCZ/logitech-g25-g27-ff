// G25FFAgent.exe: the agent (see agent.h), started at logon from the HKLM Run key. A program of its own rather
// than rundll32, so Windows and security tools see what starts and Task Manager shows its description.
#include <windows.h>
#include "../agent.h"
#include "../log.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    LogInit();
    RunAgent();
    return 0;
}
