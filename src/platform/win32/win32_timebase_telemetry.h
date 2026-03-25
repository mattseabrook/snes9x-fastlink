#ifndef WIN32_TIMEBASE_TELEMETRY_H
#define WIN32_TIMEBASE_TELEMETRY_H

#include "port.h"

struct WinTimebaseTelemetry
{
	uint64 sequence;
	double windowWallSeconds;
	double windowEmulatedSeconds;
	double windowLossMs;
	double cumulativeLossMs;
	double timebaseCorrectionMultiplier;
	double dynamicRateMultiplier;
	int64 throttleCarryDebtUs;
	double targetFrameRate;
	bool valid;
};

bool WinGetTimebaseTelemetry(WinTimebaseTelemetry *telemetry);
bool WinIsTimebaseTelemetryEnabled();

#endif