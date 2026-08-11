#pragma once

// Simple console toggle for a /SUBSYSTEM:WINDOWS app. When enabled, stdout
// and stderr are redirected to a dynamically allocated console window so
// printf/fprintf from any thread shows up in it.

void ShowDebugConsole(bool show);
bool IsDebugConsoleShown();
