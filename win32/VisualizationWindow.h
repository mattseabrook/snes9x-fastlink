#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

void VisualizationToggle(HWND parent);
void VisualizationShutdown();
bool VisualizationIsVisible();
