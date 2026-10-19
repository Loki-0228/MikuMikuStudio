#pragma once
#include <Windows.h>
namespace mikumikustudio { class MMDApp;
namespace recent_projects {
void Record(MMDApp*, const wchar_t*);
bool HandleMessage(MMDApp*, UINT, WPARAM, LPARAM);
LRESULT CALLBACK MenuFilter(int, WPARAM, LPARAM);
} }
