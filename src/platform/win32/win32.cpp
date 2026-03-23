/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "core/util/snes9x.h"
#include "core/memory/memmap.h"
#include "core/util/debug.h"
#include "core/cpu/cpuexec.h"
#include "core/ppu/ppu.h"
#include "core/state/snapshot.h"
#include "../../../src/core/apu/apu.h"
#include "core/video-common/display.h"
#include "core/video-common/gfx.h"
#include "core/state/movie.h"
#include "core/netplay/netplay.h"

#include "wsnes9x.h"
#include "win32_sound.h"
#include "win32_display.h"

#include "render.h"
#include "AVIOutput.h"
#include "wlanguage.h"

#include <shlwapi.h>
#include <direct.h>

#include <io.h>

#include <math.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <hidsdi.h>

struct SJoyState Joystick [16];
uint32 joypads [8];
bool8 do_frame_adjust=false;
static bool g_prev_joy_buttons[16][32] = {};

static BYTE g_keyboard_state[256] = {};
static SHORT g_pause_state = 0;
static BYTE g_raw_keyboard_state[256] = {};
static ULONGLONG g_raw_keyboard_ticks[256] = {};
static bool g_raw_input_registered = false;
static HWND g_raw_input_hwnd = NULL;

typedef DWORD (WINAPI *XInputGetStateProc)(DWORD, void *);

typedef struct _S9X_XINPUT_GAMEPAD {
	WORD  wButtons;
	BYTE  bLeftTrigger;
	BYTE  bRightTrigger;
	SHORT sThumbLX;
	SHORT sThumbLY;
	SHORT sThumbRX;
	SHORT sThumbRY;
} S9X_XINPUT_GAMEPAD;

typedef struct _S9X_XINPUT_STATE {
	DWORD dwPacketNumber;
	S9X_XINPUT_GAMEPAD Gamepad;
} S9X_XINPUT_STATE;

static const WORD S9X_XINPUT_GAMEPAD_DPAD_UP        = 0x0001;
static const WORD S9X_XINPUT_GAMEPAD_DPAD_DOWN      = 0x0002;
static const WORD S9X_XINPUT_GAMEPAD_DPAD_LEFT      = 0x0004;
static const WORD S9X_XINPUT_GAMEPAD_DPAD_RIGHT     = 0x0008;
static const WORD S9X_XINPUT_GAMEPAD_START          = 0x0010;
static const WORD S9X_XINPUT_GAMEPAD_BACK           = 0x0020;
static const WORD S9X_XINPUT_GAMEPAD_LEFT_THUMB     = 0x0040;
static const WORD S9X_XINPUT_GAMEPAD_RIGHT_THUMB    = 0x0080;
static const WORD S9X_XINPUT_GAMEPAD_LEFT_SHOULDER  = 0x0100;
static const WORD S9X_XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
static const WORD S9X_XINPUT_GAMEPAD_A              = 0x1000;
static const WORD S9X_XINPUT_GAMEPAD_B              = 0x2000;
static const WORD S9X_XINPUT_GAMEPAD_X              = 0x4000;
static const WORD S9X_XINPUT_GAMEPAD_Y              = 0x8000;

static XInputGetStateProc g_xinput_get_state = NULL;

static bool S9xReadXInputPad(int index, SJoyState &js);

struct S9xRawHidPad
{
	HANDLE device = NULL;
	USHORT vendor_id = 0;
	USHORT product_id = 0;
	std::vector<BYTE> preparsed;
	HIDP_CAPS caps = {};
	SJoyState state = {};
	ULONGLONG last_tick = 0;
	bool active = false;
	bool fast_path_target = false;
	// Unique DataIndex for each button — avoids duplicate-usage HID descriptor bugs
	// (e.g. RetroPort SNES adapter reports duplicate Usage IDs, but DataIndex is always unique)
	std::vector<USHORT> buttonDataIndices;
};

static std::vector<S9xRawHidPad> g_raw_hid_pads;
static SRWLOCK g_raw_hid_lock = SRWLOCK_INIT;

struct S9xFastHidPacket
{
	HANDLE device = NULL;
	std::vector<BYTE> report;
};

static std::atomic<bool> g_fast_input_running(false);
static std::thread g_fast_input_thread;
static std::mutex g_fast_input_mutex;
static std::condition_variable g_fast_input_cv;
static std::deque<S9xFastHidPacket> g_fast_input_queue;
static std::vector<DWORD> g_fast_input_targets;
static bool g_fast_input_enabled = false;
static const size_t kFastInputQueueLimit = 64;

static bool S9xInitRawHidPad(HANDLE device, S9xRawHidPad &pad);
static void S9xUpdateRawHidPad(S9xRawHidPad &pad, const RAWHID &hid);
static void S9xUpdateRawHidPadReport(S9xRawHidPad &pad, const BYTE *report, ULONG report_len);
static bool S9xGetUsageInfo(const S9xRawHidPad &pad, USAGE usage, LONG &logicalMin, LONG &logicalMax, USHORT &bitSize);
static void S9xUpdateRawHidDPad(SJoyState &js, ULONG hat);
static void S9xStartFastInputThread();
static void S9xStopFastInputThread();
static void S9xRefreshFastInputConfig();
static bool S9xIsFastInputTarget(USHORT vendor_id, USHORT product_id);
static void S9xQueueFastInputPacket(HANDLE device, const BYTE *report, ULONG report_len);
static void S9xFastInputWorkerMain();

static WORD S9xResolveRawVirtualKey(const RAWKEYBOARD &rk)
{
	WORD vkey = rk.VKey;
	if (vkey == 255)
		return 0;

	if (vkey == VK_SHIFT)
		vkey = (WORD)MapVirtualKey(rk.MakeCode, MAPVK_VSC_TO_VK_EX);
	else if (vkey == VK_CONTROL)
		vkey = (rk.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
	else if (vkey == VK_MENU)
		vkey = (rk.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;

	return vkey;
}

static void S9xRefreshFastInputConfig()
{
	g_fast_input_targets.clear();
	GUI.FastInputThread = true;
	g_fast_input_enabled = true;

	if (GUI.FastInputTargets[0] != TEXT('\0'))
	{
		const size_t len = _tcslen(GUI.FastInputTargets);
		std::vector<TCHAR> buffer(len + 1, TEXT('\0'));
		_tcscpy(buffer.data(), GUI.FastInputTargets);

		for (TCHAR *token = _tcstok(buffer.data(), TEXT(",; \t"));
			token != NULL;
			token = _tcstok(NULL, TEXT(",; \t")))
		{
			unsigned int vid = 0;
			unsigned int pid = 0;
			if (_stscanf(token, TEXT("%x:%x"), &vid, &pid) == 2 && vid <= 0xffff && pid <= 0xffff)
			{
				g_fast_input_targets.push_back(((DWORD)vid << 16) | (DWORD)pid);
			}
		}
	}

	if (g_fast_input_enabled)
		S9xStartFastInputThread();
	else
		S9xStopFastInputThread();
}

static bool S9xIsFastInputTarget(USHORT vendor_id, USHORT product_id)
{
	if (!g_fast_input_enabled)
		return false;

	if (g_fast_input_targets.empty())
		return true;

	const DWORD key = ((DWORD)vendor_id << 16) | (DWORD)product_id;
	for (DWORD target : g_fast_input_targets)
	{
		if (target == key)
			return true;
	}

	return false;
}

static void S9xQueueFastInputPacket(HANDLE device, const BYTE *report, ULONG report_len)
{
	if (!g_fast_input_running.load(std::memory_order_relaxed) || !report || report_len == 0)
		return;

	S9xFastHidPacket packet;
	packet.device = device;
	packet.report.assign(report, report + report_len);

	std::lock_guard<std::mutex> lock(g_fast_input_mutex);
	for (auto it = g_fast_input_queue.begin(); it != g_fast_input_queue.end(); ++it)
	{
		if (it->device == device)
		{
			*it = std::move(packet);
			g_fast_input_cv.notify_one();
			return;
		}
	}

	if (g_fast_input_queue.size() >= kFastInputQueueLimit)
		g_fast_input_queue.pop_front();

	g_fast_input_queue.push_back(std::move(packet));
	g_fast_input_cv.notify_one();
}

static void S9xFastInputWorkerMain()
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

	while (g_fast_input_running.load(std::memory_order_relaxed))
	{
		S9xFastHidPacket packet;
		{
			std::unique_lock<std::mutex> lock(g_fast_input_mutex);
			g_fast_input_cv.wait(lock, []()
			{
				return !g_fast_input_running.load(std::memory_order_relaxed) || !g_fast_input_queue.empty();
			});

			if (!g_fast_input_running.load(std::memory_order_relaxed))
				break;

			packet = std::move(g_fast_input_queue.front());
			g_fast_input_queue.pop_front();
		}

		AcquireSRWLockExclusive(&g_raw_hid_lock);
		for (auto &pad : g_raw_hid_pads)
		{
			if (pad.device == packet.device)
			{
				S9xUpdateRawHidPadReport(pad, packet.report.data(), (ULONG)packet.report.size());
				break;
			}
		}
		ReleaseSRWLockExclusive(&g_raw_hid_lock);
	}
}

static void S9xStartFastInputThread()
{
	if (!g_fast_input_enabled || g_fast_input_running.load(std::memory_order_relaxed))
		return;

	g_fast_input_running.store(true, std::memory_order_relaxed);
	g_fast_input_thread = std::thread(S9xFastInputWorkerMain);
}

static void S9xStopFastInputThread()
{
	if (!g_fast_input_running.load(std::memory_order_relaxed))
		return;

	g_fast_input_running.store(false, std::memory_order_relaxed);
	g_fast_input_cv.notify_all();
	if (g_fast_input_thread.joinable())
		g_fast_input_thread.join();

	std::lock_guard<std::mutex> lock(g_fast_input_mutex);
	g_fast_input_queue.clear();
}

bool S9xWinRegisterRawInput(HWND hWnd)
{
	S9xRefreshFastInputConfig();

	RAWINPUTDEVICE devices[4] = {};

	devices[0].usUsagePage = 0x01;
	devices[0].usUsage = 0x06;
	devices[0].dwFlags = RIDEV_INPUTSINK;
	devices[0].hwndTarget = hWnd;

	devices[1].usUsagePage = 0x01;
	devices[1].usUsage = 0x02;
	devices[1].dwFlags = RIDEV_INPUTSINK;
	devices[1].hwndTarget = hWnd;

	devices[2].usUsagePage = 0x01;
	devices[2].usUsage = 0x04;
	devices[2].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	devices[2].hwndTarget = hWnd;

	devices[3].usUsagePage = 0x01;
	devices[3].usUsage = 0x05;
	devices[3].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	devices[3].hwndTarget = hWnd;

	if (!RegisterRawInputDevices(devices, 4, sizeof(RAWINPUTDEVICE)))
	{
		S9xStopFastInputThread();
		g_raw_input_registered = false;
		g_raw_input_hwnd = NULL;
		return false;
	}

	g_raw_input_registered = true;
	g_raw_input_hwnd = hWnd;
	return true;
}

void S9xWinUnregisterRawInput()
{
	if (!g_raw_input_registered)
		return;

	S9xStopFastInputThread();

	RAWINPUTDEVICE devices[4] = {};
	devices[0].usUsagePage = 0x01;
	devices[0].usUsage = 0x06;
	devices[0].dwFlags = RIDEV_REMOVE;

	devices[1].usUsagePage = 0x01;
	devices[1].usUsage = 0x02;
	devices[1].dwFlags = RIDEV_REMOVE;

	devices[2].usUsagePage = 0x01;
	devices[2].usUsage = 0x04;
	devices[2].dwFlags = RIDEV_REMOVE;

	devices[3].usUsagePage = 0x01;
	devices[3].usUsage = 0x05;
	devices[3].dwFlags = RIDEV_REMOVE;

	RegisterRawInputDevices(devices, 4, sizeof(RAWINPUTDEVICE));

	g_raw_input_registered = false;
	g_raw_input_hwnd = NULL;
	memset(g_raw_keyboard_state, 0, sizeof(g_raw_keyboard_state));
	memset(g_raw_keyboard_ticks, 0, sizeof(g_raw_keyboard_ticks));
	AcquireSRWLockExclusive(&g_raw_hid_lock);
	g_raw_hid_pads.clear();
	ReleaseSRWLockExclusive(&g_raw_hid_lock);
}

void S9xWinHandleRawInput(HRAWINPUT hRawInput)
{
	if (!hRawInput)
		return;

	UINT data_size = 0;
	if (GetRawInputData(hRawInput, RID_INPUT, NULL, &data_size, sizeof(RAWINPUTHEADER)) != 0 || data_size == 0)
		return;

	std::vector<BYTE> raw_buffer(data_size);
	if (GetRawInputData(hRawInput, RID_INPUT, raw_buffer.data(), &data_size, sizeof(RAWINPUTHEADER)) != data_size)
		return;

	RAWINPUT *raw = reinterpret_cast<RAWINPUT *>(raw_buffer.data());
	if (raw->header.dwType == RIM_TYPEKEYBOARD)
	{
		const RAWKEYBOARD &rk = raw->data.keyboard;
		WORD vkey = S9xResolveRawVirtualKey(rk);
		if (vkey == 0 || vkey >= 256)
			return;

		bool is_key_up = (rk.Flags & RI_KEY_BREAK) != 0;
		g_raw_keyboard_state[vkey] = is_key_up ? 0 : 0x80;
		g_raw_keyboard_ticks[vkey] = GetTickCount64();
		return;
	}

	if (raw->header.dwType != RIM_TYPEHID)
		return;

	HANDLE device = raw->header.hDevice;
	if (!device)
		return;

	// Filter out XInput-compatible devices so the same controller doesn't
	// appear in both the XInput slots AND the Raw HID slots.
	if (g_xinput_get_state)
	{
		UINT name_size = 0;
		GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, NULL, &name_size);
		if (name_size > 0)
		{
			std::vector<char> name_buf(name_size + 1, '\0');
			if (GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, name_buf.data(), &name_size) != (UINT)-1)
			{
				// Microsoft marks XInput-capable devices with "IG_" in
				// the device interface path.
				for (char *p = name_buf.data(); *p; ++p)
					*p = (char)toupper((unsigned char)*p);
				if (strstr(name_buf.data(), "IG_") != NULL)
					return; // skip — already handled by XInput
			}
		}
	}

	const RAWHID &hid = raw->data.hid;
	const ULONG report_len = hid.dwSizeHid;
	if (report_len == 0 || hid.dwCount == 0)
		return;
	const BYTE *report = hid.bRawData + (hid.dwCount - 1) * report_len;

	AcquireSRWLockExclusive(&g_raw_hid_lock);
	for (auto &pad : g_raw_hid_pads)
	{
		if (pad.device == device)
		{
			if (pad.fast_path_target && g_fast_input_running.load(std::memory_order_relaxed))
				S9xQueueFastInputPacket(device, report, report_len);
			else
				S9xUpdateRawHidPadReport(pad, report, report_len);
			ReleaseSRWLockExclusive(&g_raw_hid_lock);
			return;
		}
	}

	S9xRawHidPad pad;
	if (!S9xInitRawHidPad(device, pad))
	{
		ReleaseSRWLockExclusive(&g_raw_hid_lock);
		return;
	}

	if (pad.fast_path_target && g_fast_input_running.load(std::memory_order_relaxed))
		S9xQueueFastInputPacket(device, report, report_len);
	else
		S9xUpdateRawHidPadReport(pad, report, report_len);

	g_raw_hid_pads.push_back(pad);
	ReleaseSRWLockExclusive(&g_raw_hid_lock);
}

// avi variables
static uint8* avi_buffer = NULL;
static uint8* avi_sound_buffer = NULL;
static int avi_sound_bytes_per_sample = 0;
static double avi_sound_samples_per_update = 0;
static double avi_sound_samples_error = 0;
static int avi_width = 0;
static int avi_height = 0;
static int avi_pitch = 0;
static int avi_image_size = 0;
static uint32 avi_skip_frames = 0;
static bool pre_avi_soundsync = true;
static uint32 pre_avi_soundinputrate = 32000;
void DoAVIOpen(const char* filename);
void DoAVIClose(int reason);

void S9xWinScanJoypads ();

typedef struct
{
    uint8 red;
    uint8 green;
    uint8 blue;
} Colour;

void ConvertDepth (SSurface *src, SSurface *dst, RECT *);
static Colour FixedColours [256];
static uint8 palette [0x10000];

FILE *trace_fs = NULL;

int __fastcall Normalize (int cur, int min, int max)
{
    int Result = 0;

    if ((max - min) == 0)
        return (Result);

    Result = cur - min;
    Result = (Result * 200) / (max - min);
    Result -= 100;

    return (Result);
}

static void S9xWinInitXInput()
{
	if (g_xinput_get_state)
		return;

	const TCHAR *xinput_dlls[] = {
		TEXT("xinput1_4.dll"),
		TEXT("xinput1_3.dll"),
		TEXT("xinput9_1_0.dll")
	};

	for (const TCHAR *dll : xinput_dlls)
	{
		HMODULE module = LoadLibrary(dll);
		if (!module)
			continue;

		g_xinput_get_state = (XInputGetStateProc)GetProcAddress(module, "XInputGetState");
		if (g_xinput_get_state)
		{
			return;
		}

		FreeLibrary(module);
	}
}

static void S9xClearJoyState(SJoyState &js)
{
	js.Left = js.Right = js.Up = js.Down = false;
	js.PovLeft = js.PovRight = js.PovUp = js.PovDown = false;
	js.PovDnLeft = js.PovDnRight = js.PovUpLeft = js.PovUpRight = false;
	js.RUp = js.RDown = js.UUp = js.UDown = js.VUp = js.VDown = false;
	js.ZUp = js.ZDown = false;
	memset(js.Button, 0, sizeof(js.Button));
}

static int S9xNormalizeSignedAxis(short value)
{
	int normalized = (value * 100) / 32767;
	return std::max(-100, std::min(100, normalized));
}

static bool S9xReadXInputPad(int index, SJoyState &js)
{
	if (!g_xinput_get_state)
		return false;

	S9X_XINPUT_STATE state{};
	if (g_xinput_get_state(index, &state) != 0)
		return false;

	js.Attached = true;
	S9xClearJoyState(js);

	const int deadzone = S9X_JOY_NEUTRAL;
	const int lx = S9xNormalizeSignedAxis(state.Gamepad.sThumbLX);
	const int ly = S9xNormalizeSignedAxis(state.Gamepad.sThumbLY);

	js.Left = lx < -deadzone;
	js.Right = lx > deadzone;
	js.Up = ly > deadzone;
	js.Down = ly < -deadzone;

	const bool dpad_up = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_DPAD_UP) != 0;
	const bool dpad_down = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_DPAD_DOWN) != 0;
	const bool dpad_left = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_DPAD_LEFT) != 0;
	const bool dpad_right = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

	js.PovUp = dpad_up && !dpad_left && !dpad_right;
	js.PovDown = dpad_down && !dpad_left && !dpad_right;
	js.PovLeft = dpad_left && !dpad_up && !dpad_down;
	js.PovRight = dpad_right && !dpad_up && !dpad_down;
	js.PovUpLeft = dpad_up && dpad_left;
	js.PovUpRight = dpad_up && dpad_right;
	js.PovDnLeft = dpad_down && dpad_left;
	js.PovDnRight = dpad_down && dpad_right;

	js.ZUp = state.Gamepad.bLeftTrigger > 30;
	js.ZDown = state.Gamepad.bRightTrigger > 30;

	js.Button[0] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_A) != 0;
	js.Button[1] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_B) != 0;
	js.Button[2] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_X) != 0;
	js.Button[3] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_Y) != 0;
	js.Button[4] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
	js.Button[5] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
	js.Button[6] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_BACK) != 0;
	js.Button[7] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_START) != 0;
	js.Button[8] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_LEFT_THUMB) != 0;
	js.Button[9] = (state.Gamepad.wButtons & S9X_XINPUT_GAMEPAD_RIGHT_THUMB) != 0;

	return true;
}

static bool S9xUpdateXInputPad(int index)
{
	if (index < 0 || index >= 16)
		return false;

	SJoyState state = {};
	if (!S9xReadXInputPad(index, state))
		return false;

	Joystick[index] = state;
	return true;
}

static bool S9xInitRawHidPad(HANDLE device, S9xRawHidPad &pad)
{
	RID_DEVICE_INFO info = {};
	info.cbSize = sizeof(info);
	UINT info_size = sizeof(info);
	if (GetRawInputDeviceInfo(device, RIDI_DEVICEINFO, &info, &info_size) == (UINT)-1)
		return false;

	if (info.dwType != RIM_TYPEHID)
		return false;

	if (info.hid.usUsagePage != 0x01)
		return false;

	if (info.hid.usUsage != 0x04 && info.hid.usUsage != 0x05)
		return false;

	UINT ppd_size = 0;
	if (GetRawInputDeviceInfo(device, RIDI_PREPARSEDDATA, NULL, &ppd_size) == (UINT)-1 || ppd_size == 0)
		return false;

	pad.preparsed.resize(ppd_size);
	if (GetRawInputDeviceInfo(device, RIDI_PREPARSEDDATA, pad.preparsed.data(), &ppd_size) == (UINT)-1)
		return false;

	PHIDP_PREPARSED_DATA ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(pad.preparsed.data());
	if (HidP_GetCaps(ppd, &pad.caps) != HIDP_STATUS_SUCCESS)
		return false;

	pad.device = device;
	pad.vendor_id = (USHORT)info.hid.dwVendorId;
	pad.product_id = (USHORT)info.hid.dwProductId;
	pad.active = true;
	pad.fast_path_target = S9xIsFastInputTarget(pad.vendor_id, pad.product_id);
	pad.state = {};

	// Enumerate button caps to build a unique DataIndex → sequential button map.
	// HidP_GetUsages() returns Usage IDs which may be duplicated in quirky
	// descriptors (e.g. RetroPort SNES). DataIndex is always unique per button.
	USHORT num_btn_caps = pad.caps.NumberInputButtonCaps;
	if (num_btn_caps > 0)
	{
		std::vector<HIDP_BUTTON_CAPS> btn_caps(num_btn_caps);
		if (HidP_GetButtonCaps(HidP_Input, btn_caps.data(), &num_btn_caps, ppd) == HIDP_STATUS_SUCCESS)
		{
			for (USHORT i = 0; i < num_btn_caps; i++)
			{
				if (btn_caps[i].UsagePage != 0x09)
					continue;
				if (btn_caps[i].IsRange)
				{
					for (USHORT d = btn_caps[i].Range.DataIndexMin; d <= btn_caps[i].Range.DataIndexMax; d++)
						pad.buttonDataIndices.push_back(d);
				}
				else
				{
					pad.buttonDataIndices.push_back(btn_caps[i].NotRange.DataIndex);
				}
			}
			std::sort(pad.buttonDataIndices.begin(), pad.buttonDataIndices.end());
		}
	}

	return true;
}

static bool S9xGetUsageInfo(const S9xRawHidPad &pad, USAGE usage, LONG &logicalMin, LONG &logicalMax, USHORT &bitSize)
{
	USHORT value_caps_len = pad.caps.NumberInputValueCaps;
	if (value_caps_len == 0)
		return false;

	std::vector<HIDP_VALUE_CAPS> value_caps(value_caps_len);
	PHIDP_PREPARSED_DATA ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(const_cast<BYTE *>(pad.preparsed.data()));
	if (HidP_GetValueCaps(HidP_Input, value_caps.data(), &value_caps_len, ppd) != HIDP_STATUS_SUCCESS)
		return false;

	for (USHORT i = 0; i < value_caps_len; i++)
	{
		if (value_caps[i].UsagePage != 0x01)
			continue;

		USAGE min_usage = value_caps[i].IsRange ? value_caps[i].Range.UsageMin : value_caps[i].NotRange.Usage;
		USAGE max_usage = value_caps[i].IsRange ? value_caps[i].Range.UsageMax : value_caps[i].NotRange.Usage;
		if (usage < min_usage || usage > max_usage)
			continue;

		logicalMin = value_caps[i].LogicalMin;
		logicalMax = value_caps[i].LogicalMax;
		bitSize = value_caps[i].BitSize;
		if (logicalMax <= logicalMin)
			return false;
		return true;
	}

	return false;
}

static void S9xUpdateRawHidDPad(SJoyState &js, ULONG hat)
{
	js.PovUp = js.PovDown = js.PovLeft = js.PovRight = false;
	js.PovUpLeft = js.PovUpRight = js.PovDnLeft = js.PovDnRight = false;

	switch (hat)
	{
		case 0: js.PovUp = true; break;
		case 1: js.PovUpRight = true; break;
		case 2: js.PovRight = true; break;
		case 3: js.PovDnRight = true; break;
		case 4: js.PovDown = true; break;
		case 5: js.PovDnLeft = true; break;
		case 6: js.PovLeft = true; break;
		case 7: js.PovUpLeft = true; break;
		default: break;
	}

	if (js.PovUp || js.PovUpLeft || js.PovUpRight)
		js.Up = true;
	if (js.PovDown || js.PovDnLeft || js.PovDnRight)
		js.Down = true;
	if (js.PovLeft || js.PovUpLeft || js.PovDnLeft)
		js.Left = true;
	if (js.PovRight || js.PovUpRight || js.PovDnRight)
		js.Right = true;
}

static void S9xUpdateRawHidPad(S9xRawHidPad &pad, const RAWHID &hid)
{
	const ULONG report_len = hid.dwSizeHid;
	if (report_len == 0 || hid.dwCount == 0)
		return;

	const BYTE *report = hid.bRawData + (hid.dwCount - 1) * report_len;
	S9xUpdateRawHidPadReport(pad, report, report_len);
}

static void S9xUpdateRawHidPadReport(S9xRawHidPad &pad, const BYTE *report, ULONG report_len)
{
	if (!report || report_len == 0)
		return;

	PHIDP_PREPARSED_DATA ppd = reinterpret_cast<PHIDP_PREPARSED_DATA>(pad.preparsed.data());

	SJoyState js = {};
	js.Attached = true;

	// Use HidP_GetData (DataIndex-based) instead of HidP_GetUsages (Usage-based).
	// DataIndex is always unique per button, even when the HID descriptor has
	// duplicate Usage IDs (e.g. RetroPort SNES adapter quirk).
	ULONG max_data = HidP_MaxDataListLength(HidP_Input, ppd);
	if (max_data > 0 && !pad.buttonDataIndices.empty())
	{
		std::vector<HIDP_DATA> data_list(max_data);
		ULONG data_len = max_data;
		if (HidP_GetData(HidP_Input, data_list.data(), &data_len, ppd, (PCHAR)report, report_len) == HIDP_STATUS_SUCCESS)
		{
			for (ULONG i = 0; i < data_len; i++)
			{
				// Check if this DataIndex corresponds to a button
				auto it = std::lower_bound(pad.buttonDataIndices.begin(), pad.buttonDataIndices.end(), data_list[i].DataIndex);
				if (it != pad.buttonDataIndices.end() && *it == data_list[i].DataIndex)
				{
					int btn_index = (int)(it - pad.buttonDataIndices.begin());
					if (btn_index >= 0 && btn_index < 32 && data_list[i].On)
						js.Button[btn_index] = true;
				}
			}
		}
	}

	LONG logical_min = 0;
	LONG logical_max = 0;
	USHORT bit_size = 0;
	ULONG value = 0;

	auto read_axis = [&](USAGE usage, bool &negative, bool &positive)
	{
		if (!S9xGetUsageInfo(pad, usage, logical_min, logical_max, bit_size))
			return;

		if (logical_max <= logical_min)
			return;

		if (HidP_GetUsageValue(HidP_Input, 0x01, 0, usage, &value, ppd, (PCHAR)report, report_len) != HIDP_STATUS_SUCCESS)
			return;

		LONG signed_value = value;
		if (logical_min < 0 && bit_size > 0 && (value & (1UL << (bit_size - 1))))
			signed_value = value | (~0UL << bit_size);

		int norm = ((int)(signed_value - logical_min) * 200 / (logical_max - logical_min)) - 100;
		negative = norm < -S9X_JOY_NEUTRAL;
		positive = norm > S9X_JOY_NEUTRAL;
	};

	read_axis(0x30, js.Left, js.Right);
	read_axis(0x31, js.Up, js.Down);
	read_axis(0x32, js.ZUp, js.ZDown);
	read_axis(0x33, js.RUp, js.RDown);
	read_axis(0x34, js.UUp, js.UDown);
	read_axis(0x35, js.VUp, js.VDown);

	if (S9xGetUsageInfo(pad, 0x39, logical_min, logical_max, bit_size) &&
		HidP_GetUsageValue(HidP_Input, 0x01, 0, 0x39, &value, ppd, (PCHAR)report, report_len) == HIDP_STATUS_SUCCESS)
	{
		LONG signed_value = value;
		if (logical_min < 0 && (value & (1UL << (bit_size - 1))))
			signed_value = value | (~0UL << bit_size);

		if (signed_value >= logical_min && signed_value <= logical_max)
		{
			ULONG hat = signed_value - logical_min;
			S9xUpdateRawHidDPad(js, hat);
		}
	}

	pad.state = js;
	pad.last_tick = GetTickCount64();
	pad.active = true;
}

void S9xWinUpdateKeyState()
{
	if (g_raw_input_registered)
	{
		memcpy(g_keyboard_state, g_raw_keyboard_state, sizeof(g_keyboard_state));
	}
	else
	{
		if (!GetKeyboardState(g_keyboard_state))
			memset(g_keyboard_state, 0, sizeof(g_keyboard_state));
		else
		{
			for (int i = 0; i < 256; i++)
				g_keyboard_state[i] &= 0x80;
		}
	}

	g_pause_state = (g_keyboard_state[VK_PAUSE] & 0x80) ? 1 : 0;
}

void S9xTextMode( void)
{
}

void S9xGraphicsMode ()
{
}

void S9xExit( void)
{
    SendMessage (GUI.hWnd, WM_COMMAND, ID_FILE_EXIT, 0);
}

#define IS_SLASH(x) ((x) == TEXT('\\') || (x) == TEXT('/'))
static TCHAR startDirectory [PATH_MAX];
static bool startDirectoryValid = false;

const TCHAR *S9xGetDirectoryT (enum s9x_getdirtype dirtype)
{
    static TCHAR filename[PATH_MAX];
	if(!startDirectoryValid)
	{
		// directory of the executable's location:
		GetModuleFileName(NULL, startDirectory, PATH_MAX);
        for(int i=lstrlen(startDirectory); i>=0; i--){
            if(IS_SLASH(startDirectory[i])){
                startDirectory[i]=TEXT('\0');
                break;
            }
        }

		startDirectoryValid = true;
	}

	const TCHAR* rv = startDirectory;

    switch(dirtype){
	  default:
      case DEFAULT_DIR:
	  case HOME_DIR:
		  break;

	  case SCREENSHOT_DIR:
		  rv = GUI.ScreensDir;
		  break;

      case ROM_DIR:
		  rv = GUI.RomDir;
		  break;

      case SRAM_DIR:
		  rv = GUI.SRAMFileDir;
		  break;

	  case BIOS_DIR:
		  rv = GUI.BiosDir;
		  break;

      case SPC_DIR:
		  rv = GUI.SPCDir;
		  break;

	  case PATCH_DIR:
		  rv = GUI.PatchDir;
		  break;

	  case CHEAT_DIR:
		  rv = GUI.CheatDir;
		  break;

	  case SNAPSHOT_DIR:
		  rv = GUI.FreezeFileDir;
		  break;

	  case SAT_DIR:
		  rv = GUI.SatDir;
		  break;

	  case ROMFILENAME_DIR: {
			lstrcpy(filename, _tFromChar(Memory.ROMFilename.c_str()));
			if(!filename[0])
				rv = GUI.RomDir;
			for(int i=lstrlen(filename); i>=0; i--){
				if(IS_SLASH(filename[i])){
					filename[i]=TEXT('\0');
					break;
				}
			}
			rv = filename;
		}
		break;
    }

    if (PathIsRelative(rv)) {
        TCHAR temp_container[PATH_MAX];
        _sntprintf(temp_container, PATH_MAX, TEXT("%s\\%s"), startDirectory, rv);
        GetFullPathName(temp_container, PATH_MAX, filename, NULL);
        rv = filename;
    }

	_tmkdir(rv);

	return rv;
}

std::string S9xGetDirectory (enum s9x_getdirtype dirtype)
{
	return std::string(_tToChar(S9xGetDirectoryT(dirtype)));
}

std::string S9xGetFilenameInc (std::string e, enum s9x_getdirtype dirtype)
{
    std::string filename;

    auto split = splitpath(Memory.ROMFilename);
    std::string directory_string = S9xGetDirectory(dirtype);

    unsigned int i = 0;
    do {
        std::string new_extension = std::to_string(i);
        while (new_extension.length() < 3)
            new_extension = "0" + new_extension;
        new_extension += e;

        filename = makepath("", directory_string, split.stem, new_extension);
        i++;
    } while(_taccess(_tFromChar(filename.c_str()), 0) == 0 && i < 1000);

    return (filename);
}

bool8 S9xOpenSnapshotFile( const char *fname, bool8 read_only, STREAM *file)
{
    char filename [_MAX_PATH + 1];
    char drive [_MAX_DRIVE + 1];
    char dir [_MAX_DIR + 1];
    char fn [_MAX_FNAME + 1];
    char ext [_MAX_EXT + 1];

    _splitpath( fname, drive, dir, fn, ext);
    _makepath( filename, drive, dir, fn, ext[0] == '\0' ? ".000" : ext);

    if (read_only)
    {
	if ((*file = OPEN_STREAM (filename, "rb")))
	    return (TRUE);
    }
    else
    {
	if ((*file = OPEN_STREAM (filename, "wb")))
	    return (TRUE);
        FILE *fs = fopen (filename, "rb");
        if (fs)
        {
            sprintf (String, "Freeze file \"%s\" exists but is read only",
                     filename);
            fclose (fs);
            S9xMessage (S9X_ERROR, S9X_FREEZE_FILE_NOT_FOUND, String);
        }
        else
        {
            sprintf (String, "Cannot create freeze file \"%s\". Directory is read-only or does not exist.", filename);

            S9xMessage (S9X_ERROR, S9X_FREEZE_FILE_NOT_FOUND, String);
        }
    }
    return (FALSE);
}

void S9xCloseSnapshotFile( STREAM file)
{
    CLOSE_STREAM (file);
}

void S9xMessage (int type, int, const char *str)
{
#ifdef DEBUGGER
    static FILE *out = NULL;

    if (out == NULL)
        out = fopen ("out.txt", "w");

    fprintf (out, "%s\n", str);
#endif

    S9xSetInfoString (str);

	// if we can't draw on the screen, messagebox it
	// also send to stderr/stdout depending on message type
	switch(type)
	{
		case S9X_INFO:
			if(Settings.StopEmulation)
				fprintf(stdout, "%s\n", str);
			break;
		case S9X_WARNING:
			fprintf(stdout, "%s\n", str);
			if(Settings.StopEmulation)
				MessageBoxA(GUI.hWnd, str, "Warning",     MB_OK | MB_ICONWARNING);
			break;
		case S9X_ERROR:
			fprintf(stderr, "%s\n", str);
			if(Settings.StopEmulation)
				MessageBoxA(GUI.hWnd, str, "Error",       MB_OK | MB_ICONERROR);
			break;
		case S9X_FATAL_ERROR:
			fprintf(stderr, "%s\n", str);
			if(Settings.StopEmulation)
				MessageBoxA(GUI.hWnd, str, "Fatal Error", MB_OK | MB_ICONERROR);
			break;
		default:
				fprintf(stdout, "%s\n", str);
			break;
	}
}

extern unsigned long START;

void S9xSyncSpeed( void)
{
#ifdef NETPLAY_SUPPORT
    if (Settings.NetPlay)
    {
#if defined (NP_DEBUG) && NP_DEBUG == 2
        printf ("CLIENT: SyncSpeed @%d\n", timeGetTime () - START);
#endif
        S9xWinScanJoypads ();

		LONG prev;
        BOOL success;

	// Wait for heart beat from server
        if ((success = ReleaseSemaphore (GUI.ClientSemaphore, 1, &prev)) &&
            prev == 0)
        {
            // No heartbeats already arrived, have to wait for one.
            // Mop up the ReleaseSemaphore test above...
            WaitForSingleObject (GUI.ClientSemaphore, 0);

            // ... and then wait for the real sync-signal from the
            // client loop thread.
            NetPlay.PendingWait4Sync = WaitForSingleObject (GUI.ClientSemaphore, 100) != WAIT_OBJECT_0;
#if defined (NP_DEBUG) && NP_DEBUG == 2
            if (NetPlay.PendingWait4Sync)
                printf ("CLIENT: PendingWait4Sync1 @%d\n", timeGetTime () - START);
#endif
            IPPU.RenderThisFrame = TRUE;
            IPPU.SkippedFrames = 0;
        }
        else
        {
            if (success)
            {
                // Once for the ReleaseSemaphore above...
                WaitForSingleObject (GUI.ClientSemaphore, 0);
                if (prev == 4 && NetPlay.Waiting4EmulationThread)
                {
                    // Reached the lower behind count threshold - tell the
                    // server its safe to start sending sync pulses again.
                    NetPlay.Waiting4EmulationThread = FALSE;
                    S9xNPSendPause (FALSE);
                }

#if defined (NP_DEBUG) && NP_DEBUG == 2
                if (prev > 1)
                {
                    printf ("CLIENT: SyncSpeed prev: %d @%d\n", prev, timeGetTime () - START);
                }
#endif
            }
            else
            {
#ifdef NP_DEBUG
                printf ("*** CLIENT: SyncSpeed: Release failed @ %d\n", timeGetTime () - START);
#endif
            }

            // ... and again to mop up the already-waiting sync-signal
            NetPlay.PendingWait4Sync = WaitForSingleObject (GUI.ClientSemaphore, 200) != WAIT_OBJECT_0;
#if defined (NP_DEBUG) && NP_DEBUG == 2
            if (NetPlay.PendingWait4Sync)
                printf ("CLIENT: PendingWait4Sync2 @%d\n", timeGetTime () - START);
#endif

	    if (IPPU.SkippedFrames < NetPlay.MaxFrameSkip)
	    {
		IPPU.SkippedFrames++;
		IPPU.RenderThisFrame = FALSE;
	    }
	    else
	    {
		IPPU.RenderThisFrame = TRUE;
		IPPU.SkippedFrames = 0;
	    }
        }
        // Give up remainder of time-slice to any other waiting threads,
        // if they need any time, that is.
        Sleep (0);
        if (!NetPlay.PendingWait4Sync)
        {
            NetPlay.FrameCount++;
            S9xNPStepJoypadHistory ();
        }
    }
    else
#endif

    if (!Settings.TurboMode && Settings.SkipFrames == AUTO_FRAMERATE &&
		!GUI.AVIOut)
    {
		if (!do_frame_adjust)
		{
			IPPU.RenderThisFrame = TRUE;
			IPPU.SkippedFrames = 0;
		}
		else
		{
			if (IPPU.SkippedFrames < Settings.AutoMaxSkipFrames)
			{
				IPPU.SkippedFrames++;
				IPPU.RenderThisFrame = FALSE;
			}
			else
			{
				IPPU.RenderThisFrame = TRUE;
				IPPU.SkippedFrames = 0;
			}
		}
	}
    else
    {
	uint32 SkipFrames;
	if(Settings.TurboMode && !GUI.AVIOut)
		SkipFrames = Settings.TurboSkipFrames;
	else
		SkipFrames = (Settings.SkipFrames == AUTO_FRAMERATE) ? 0 : Settings.SkipFrames;
	if (IPPU.FrameSkip++ >= SkipFrames)
	{
	    IPPU.FrameSkip = 0;
	    IPPU.SkippedFrames = 0;
	    IPPU.RenderThisFrame = TRUE;
	}
	else
	{
	    IPPU.SkippedFrames++;
		IPPU.RenderThisFrame = GUI.AVIOut!=0;
	}
    }
}

const char *S9xBasename (const char *f)
{
	const char *p = f;
	const char *last = p;
	const char *slash;
	const char *backslash;

	// search rightmost separator
	while (true)
	{
		slash = strchr (p, '/');
		backslash = strchr (p, '\\');
		if (backslash != NULL)
		{
			if (slash == NULL || slash > backslash)
			{
				slash = backslash;
			}
		}
		if (slash == NULL)
		{
			break;
		}

		p = slash + 1;

#ifdef UNICODE
		// update always; UTF-8 doesn't have a problem between ASCII character and multi-byte character.
		last = p;
#else
		// update if it's not a trailer byte of a double-byte character.
		if (CharPrev(f, p) == slash)
		{
			last = p;
		}
#endif
	}

	return last;
}

bool8 S9xReadMousePosition (int which, int &x, int &y, uint32 &buttons)
{
    if (which == 0)
    {
        x = GUI.MouseX;
        y = GUI.MouseY;
        buttons = GUI.MouseButtons;
        return (TRUE);
    }

    return (FALSE);
}

bool S9xGetState (WORD KeyIdent)
{
	if(KeyIdent == 0 || KeyIdent == VK_ESCAPE) // if it's the 'disabled' key, it's never pressed
		return true;

	if(!GUI.BackgroundInput)
	{
		static DWORD lastFocusTick = 0;
		static bool isForeground = true;
		DWORD tick = GetTickCount();
		if (tick != lastFocusTick)
		{
			lastFocusTick = tick;
			isForeground = (GUI.hWnd == GetForegroundWindow());
		}
		if(!isForeground)
			return true;
	}

    if (KeyIdent & 0x8000) // if it's a joystick 'key':
    {
        int j = (KeyIdent >> 8) & 15;

        switch (KeyIdent & 0xff)
        {
            case 0: return !Joystick [j].Left;
            case 1: return !Joystick [j].Right;
            case 2: return !Joystick [j].Up;
            case 3: return !Joystick [j].Down;
            case 4: return !Joystick [j].PovLeft;
            case 5: return !Joystick [j].PovRight;
            case 6: return !Joystick [j].PovUp;
            case 7: return !Joystick [j].PovDown;
			case 49:return !Joystick [j].PovDnLeft;
			case 50:return !Joystick [j].PovDnRight;
			case 51:return !Joystick [j].PovUpLeft;
			case 52:return !Joystick [j].PovUpRight;
            case 41:return !Joystick [j].ZUp;
            case 42:return !Joystick [j].ZDown;
            case 43:return !Joystick [j].RUp;
            case 44:return !Joystick [j].RDown;
            case 45:return !Joystick [j].UUp;
            case 46:return !Joystick [j].UDown;
            case 47:return !Joystick [j].VUp;
            case 48:return !Joystick [j].VDown;

            default:
                if ((KeyIdent & 0xff) > 40)
                    return true; // not pressed

                return !Joystick [j].Button [(KeyIdent & 0xff) - 8];
        }
    }

	// the pause key is special, need this to catch all presses of it
	if(KeyIdent == VK_PAUSE)
	{
		if(g_pause_state) // not &'ing this with 0x8000 is intentional and necessary
			return false;
	}

	if (KeyIdent < 256)
		return ((g_keyboard_state[KeyIdent] & 0x80) == 0);

	return ((GetAsyncKeyState(KeyIdent) & 0x8000) == 0);
}

void S9xWinScanJoypads ()
{
	S9xWinUpdateKeyState();

	auto log_joy_button_press = [](int joy_index, int button_index)
	{
		char logbuf[128];
		_snprintf(logbuf, sizeof(logbuf), "[SNES9X-RUNTIME] J%d Button%d pressed\n", joy_index, button_index);
		logbuf[sizeof(logbuf) - 1] = '\0';
		OutputDebugStringA(logbuf);

		char module_path[MAX_PATH] = {};
		if (GetModuleFileNameA(NULL, module_path, MAX_PATH) > 0)
		{
			char *slash = strrchr(module_path, '\\');
			if (slash)
				slash[1] = '\0';
			strncat(module_path, "snes9x_runtime.log", MAX_PATH - strlen(module_path) - 1);

			HANDLE h = CreateFileA(module_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
				NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h != INVALID_HANDLE_VALUE)
			{
				DWORD written = 0;
				WriteFile(h, logbuf, (DWORD)strlen(logbuf), &written, NULL);
				CloseHandle(h);
			}
		}
	};

    uint8 PadState[2];

	for (int c = 0; c != 16; c++)
	{
		if (c < 4 && S9xUpdateXInputPad(c))
			continue;

		S9xClearJoyState(Joystick[c]);
		Joystick[c].Attached = false;
	}

	int slot = 0;
	std::vector<SJoyState> raw_hid_states;
	AcquireSRWLockShared(&g_raw_hid_lock);
	raw_hid_states.reserve(g_raw_hid_pads.size());
	for (const auto &pad : g_raw_hid_pads)
	{
		if (!pad.active)
			continue;
		raw_hid_states.push_back(pad.state);
	}
	ReleaseSRWLockShared(&g_raw_hid_lock);

	for (size_t i = 0; i < raw_hid_states.size(); i++)
	{

		while (slot < 16 && Joystick[slot].Attached)
			slot++;

		if (slot >= 16)
			break;

		Joystick[slot] = raw_hid_states[i];
		Joystick[slot].Attached = true;
		slot++;
	}

	for (int j = 0; j < 16; j++)
	{
		for (int b = 0; b < 32; b++)
		{
			bool pressed = Joystick[j].Attached && Joystick[j].Button[b];
			if (pressed && !g_prev_joy_buttons[j][b])
				log_joy_button_press(j, b);
			g_prev_joy_buttons[j][b] = pressed;
		}
	}

    for (int J = 0; J < 8; J++)
    {
        if (Joypad [J].Enabled)
        {
			// toggle checks
			{
       	     	PadState[0]  = 0;
				PadState[0] |= ToggleJoypadStorage[J].R||TurboToggleJoypadStorage[J].R      ?  16 : 0;
				PadState[0] |= ToggleJoypadStorage[J].L||TurboToggleJoypadStorage[J].L      ?  32 : 0;
				PadState[0] |= ToggleJoypadStorage[J].X||TurboToggleJoypadStorage[J].X      ?  64 : 0;
				PadState[0] |= ToggleJoypadStorage[J].A||TurboToggleJoypadStorage[J].A      ? 128 : 0;

	            PadState[1]  = 0;
				PadState[1] |= ToggleJoypadStorage[J].Right||TurboToggleJoypadStorage[J].Right   ?   1 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Left||TurboToggleJoypadStorage[J].Left     ?   2 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Down||TurboToggleJoypadStorage[J].Down     ?   4 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Up||TurboToggleJoypadStorage[J].Up         ?   8 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Start||TurboToggleJoypadStorage[J].Start   ?  16 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Select||TurboToggleJoypadStorage[J].Select ?  32 : 0;
				PadState[1] |= ToggleJoypadStorage[J].Y||TurboToggleJoypadStorage[J].Y           ?  64 : 0;
				PadState[1] |= ToggleJoypadStorage[J].B||TurboToggleJoypadStorage[J].B           ? 128 : 0;
			}
			// auto-hold AND regular key/joystick presses
			if(S9xGetState(Joypad[J+8].Autohold))
			{
				PadState[0] ^= (!S9xGetState(Joypad[J].R)||!S9xGetState(Joypad[J+8].R))      ?  16 : 0;
				PadState[0] ^= (!S9xGetState(Joypad[J].L)||!S9xGetState(Joypad[J+8].L))      ?  32 : 0;
				PadState[0] ^= (!S9xGetState(Joypad[J].X)||!S9xGetState(Joypad[J+8].X))      ?  64 : 0;
				PadState[0] ^= (!S9xGetState(Joypad[J].A)||!S9xGetState(Joypad[J+8].A))      ? 128 : 0;

				PadState[1] ^= (!S9xGetState(Joypad[J].Right))      ? 1     : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Right_Up))   ? 1 + 8 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Right_Down)) ? 1 + 4 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Left))       ? 2     : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Left_Up))    ? 2 + 8 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Left_Down))  ? 2 + 4 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Down))       ?     4 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Up))         ?     8 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Start)||!S9xGetState(Joypad[J+8].Start))   ?  16 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Select)||!S9xGetState(Joypad[J+8].Select)) ?  32 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].Y)||!S9xGetState(Joypad[J+8].Y))           ?  64 : 0;
				PadState[1] ^= (!S9xGetState(Joypad[J].B)||!S9xGetState(Joypad[J+8].B))           ? 128 : 0;
			}

			bool turbofy = !S9xGetState(Joypad[J+8].TempTurbo); // All Mod for turbo

			//handle turbo case! (autofire / auto-fire)
			if(turbofy || ((GUI.TurboMask&TURBO_A_MASK))&&(PadState[0]&128) || !S9xGetState(Joypad[J+8].A      )) PadState[0]^=(joypads[J]&128);
			if(turbofy || ((GUI.TurboMask&TURBO_B_MASK))&&(PadState[1]&128) || !S9xGetState(Joypad[J+8].B      )) PadState[1]^=((joypads[J]&(128<<8))>>8);
			if(turbofy || ((GUI.TurboMask&TURBO_Y_MASK))&&(PadState[1]&64) || !S9xGetState(Joypad[J+8].Y       )) PadState[1]^=((joypads[J]&(64<<8))>>8);
			if(turbofy || ((GUI.TurboMask&TURBO_X_MASK))&&(PadState[0]&64) || !S9xGetState(Joypad[J+8].X       )) PadState[0]^=(joypads[J]&64);
			if(turbofy || ((GUI.TurboMask&TURBO_L_MASK))&&(PadState[0]&32) || !S9xGetState(Joypad[J+8].L       )) PadState[0]^=(joypads[J]&32);
			if(turbofy || ((GUI.TurboMask&TURBO_R_MASK))&&(PadState[0]&16) || !S9xGetState(Joypad[J+8].R       )) PadState[0]^=(joypads[J]&16);
			if(turbofy || ((GUI.TurboMask&TURBO_STA_MASK))&&(PadState[1]&16) || !S9xGetState(Joypad[J+8].Start )) PadState[1]^=((joypads[J]&(16<<8))>>8);
			if(turbofy || ((GUI.TurboMask&TURBO_SEL_MASK))&&(PadState[1]&32) || !S9xGetState(Joypad[J+8].Select)) PadState[1]^=((joypads[J]&(32<<8))>>8);
			if(           ((GUI.TurboMask&TURBO_LEFT_MASK))&&(PadState[1]&2)                                    ) PadState[1]^=((joypads[J]&(2<<8))>>8);
			if(           ((GUI.TurboMask&TURBO_UP_MASK))&&(PadState[1]&8)                                      ) PadState[1]^=((joypads[J]&(8<<8))>>8);
			if(           ((GUI.TurboMask&TURBO_RIGHT_MASK))&&(PadState[1]&1)                                   ) PadState[1]^=((joypads[J]&(1<<8))>>8);
			if(           ((GUI.TurboMask&TURBO_DOWN_MASK))&&(PadState[1]&4)                                    ) PadState[1]^=((joypads[J]&(4<<8))>>8);

			if(TurboToggleJoypadStorage[J].A     ) PadState[0]^=(joypads[J]&128);
			if(TurboToggleJoypadStorage[J].B     ) PadState[1]^=((joypads[J]&(128<<8))>>8);
			if(TurboToggleJoypadStorage[J].Y     ) PadState[1]^=((joypads[J]&(64<<8))>>8);
			if(TurboToggleJoypadStorage[J].X     ) PadState[0]^=(joypads[J]&64);
			if(TurboToggleJoypadStorage[J].L     ) PadState[0]^=(joypads[J]&32);
			if(TurboToggleJoypadStorage[J].R     ) PadState[0]^=(joypads[J]&16);
			if(TurboToggleJoypadStorage[J].Start ) PadState[1]^=((joypads[J]&(16<<8))>>8);
			if(TurboToggleJoypadStorage[J].Select) PadState[1]^=((joypads[J]&(32<<8))>>8);
			if(TurboToggleJoypadStorage[J].Left  ) PadState[1]^=((joypads[J]&(2<<8))>>8);
			if(TurboToggleJoypadStorage[J].Up    ) PadState[1]^=((joypads[J]&(8<<8))>>8);
			if(TurboToggleJoypadStorage[J].Right ) PadState[1]^=((joypads[J]&(1<<8))>>8);
			if(TurboToggleJoypadStorage[J].Down  ) PadState[1]^=((joypads[J]&(4<<8))>>8);
			//end turbo case...


			// enforce left+right/up+down disallowance here to
			// avoid recording unused l+r/u+d that will cause desyncs
			// when played back with l+r/u+d is allowed
			if(!Settings.UpAndDown)
			{
				if((PadState[1] & 2) != 0)
					PadState[1] &= ~(1);
				if((PadState[1] & 8) != 0)
					PadState[1] &= ~(4);
			}

            joypads [J] = PadState [0] | (PadState [1] << 8) | 0x80000000;
        }
        else
            joypads [J] = 0;
    }
#ifdef NETPLAY_SUPPORT
    if (Settings.NetPlay)
	{
		// Send joypad position update to server
		S9xNPSendJoypadUpdate (joypads [GUI.NetplayUseJoypad1 ? 0 : NetPlay.Player-1]);

		// set input from network
		for (int J = 0; J < NP_MAX_CLIENTS; J++)
			joypads[J] = S9xNPGetJoypad (J);
	}
#endif
}

void S9xDetectJoypads()
{
	S9xWinInitXInput();
	S9xRefreshFastInputConfig();
	AcquireSRWLockExclusive(&g_raw_hid_lock);
	g_raw_hid_pads.clear();
	ReleaseSRWLockExclusive(&g_raw_hid_lock);

    for (int C = 0; C != 16; C ++)
	{
		if (C < 4 && S9xUpdateXInputPad(C))
			continue;

		S9xClearJoyState(Joystick[C]);
		Joystick[C].Attached = false;
	}
}

void InitSnes9x( void)
{
#ifdef DEBUGGER
//    extern FILE *trace;

//    trace = fopen( "SNES9X.TRC", "wt");
//    freopen( "SNES9X.OUT", "wt", stdout);
//    freopen( "SNES9X.ERR", "wt", stderr);

//    CPU.Flags |= TRACE_FLAG;
//    APU.Flags |= TRACE_FLAG;
#endif

//#ifdef GENERATE_OFFSETS_H
//    offsets_h = fopen ("offsets.h", "wt");
//    generate_offsets_h (0, NULL);
//    fclose (offsets_h);
//#endif
	S9xCustomDisplayString = S9xWinDisplayString;
    Memory.Init();

	extern void S9xPostRomInit();
	Memory.PostRomInitFunc = S9xPostRomInit;

	InitializeCriticalSection(&GUI.SoundCritSect);
    GUI.SoundSyncEvent = CreateEvent(NULL,TRUE,TRUE,NULL);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    S9xInitAPU();

	WinDisplayReset();
	ReInitSound();

	S9xMovieInit ();

    S9xDetectJoypads();
}
void DeinitS9x()
{
	S9xWinUnregisterRawInput();
	DeleteCriticalSection(&GUI.SoundCritSect);
    CloseHandle(GUI.SoundSyncEvent);
	CoUninitialize();
	if(GUI.GunSight)
		DestroyCursor(GUI.GunSight);//= LoadCursor (hInstance, MAKEINTRESOURCE (IDC_CURSOR_SCOPE));
    if(GUI.Arrow)
		DestroyCursor(GUI.Arrow);// = LoadCursor (NULL, IDC_ARROW);
	if(GUI.Accelerators)
		DestroyAcceleratorTable(GUI.Accelerators);// = LoadAccelerators (hInstance, MAKEINTRESOURCE (IDR_SNES9X_ACCELERATORS));
}

//void Convert8To24 (SSurface *src, SSurface *dst, RECT *srect)
//{
//    uint32 brightness = IPPU.MaxBrightness >> 1;
//    uint8 levels [32];
//    int height = srect->bottom - srect->top;
//    int width = srect->right - srect->left;
//    int offset1 = srect->top * src->Pitch + srect->left;
//    int offset2 = ((dst->Height - height) >> 1) * dst->Pitch +
//        ((dst->Width - width) >> 1) * 3;
//
//    for (int l = 0; l < 32; l++)
//	levels [l] = l * brightness;
//
//    for (int y = 0; y < height; y++)
//    {
//        uint8 *s = ((uint8 *) src->Surface + y * src->Pitch + offset1);
//        uint8 *d = ((uint8 *) dst->Surface + y * dst->Pitch + offset2);
//
//#ifdef LSB_FIRST
//        if (GUI.RedShift < GUI.BlueShift)
//#else
//	if (GUI.RedShift > GUI.BlueShift)
//#endif
//        {
//            // Order is RGB
//            for (int x = 0; x < width; x++)
//            {
//                uint16 pixel = PPU.CGDATA [*s++];
//                *(d + 0) = levels [(pixel & 0x1f)];
//                *(d + 1) = levels [((pixel >> 5) & 0x1f)];
//                *(d + 2) = levels [((pixel >> 10) & 0x1f)];
//                d += 3;
//            }
//        }
//        else
//        {
//            // Order is BGR
//            for (int x = 0; x < width; x++)
//            {
//                uint16 pixel = PPU.CGDATA [*s++];
//                *(d + 0) = levels [((pixel >> 10) & 0x1f)];
//                *(d + 1) = levels [((pixel >> 5) & 0x1f)];
//                *(d + 2) = levels [(pixel & 0x1f)];
//                d += 3;
//            }
//        }
//    }
//}


void S9xAutoSaveSRAM ()
{
    Memory.SaveSRAM (S9xGetFilename (".srm", SRAM_DIR).c_str());
}

void S9xSetPause (uint32 mask)
{
    Settings.ForcedPause |= mask;
	S9xSetSoundMute(TRUE);
}

void S9xClearPause (uint32 mask)
{
    Settings.ForcedPause &= ~mask;
    if (!Settings.ForcedPause)
    {
        // Wake up the main loop thread just if its blocked in a GetMessage call.
        PostMessage (GUI.hWnd, WM_NULL, 0, 0);
    }
}

bool JustifierOffscreen()
{
	return (bool)((GUI.MouseButtons&2)!=0);
}

//void JustifierButtons(uint32& justifiers)
//{
//	if(IPPU.Controller==SNES_JUSTIFIER_2)
//	{
//		if((GUI.MouseButtons&1)||(GUI.MouseButtons&2))
//		{
//			justifiers|=0x00200;
//		}
//		if(GUI.MouseButtons&4)
//		{
//			justifiers|=0x00800;
//		}
//	}
//	else
//	{
//		if((GUI.MouseButtons&1)||(GUI.MouseButtons&2))
//		{
//			justifiers|=0x00100;
//		}
//		if(GUI.MouseButtons&4)
//		{
//			justifiers|=0x00400;
//		}
//	}
//}

#define Interp(c1, c2) \
	(c1 == c2) ? c1 : \
	(((((c1 & 0x07E0)      + (c2 & 0x07E0)) >> 1) & 0x07E0) + \
	((((c1 & 0xF81F)      + (c2 & 0xF81F)) >> 1) & 0xF81F))

// Src: GFX.Screen (variable size) 16bpp top-down
// Dst: avi_buffer 256xH     (1x1) 24bpp bottom-up
void BuildAVIVideoFrame1X (void)
{
	const int srcWidth = IPPU.RenderedScreenWidth;
	const int srcHeight = IPPU.RenderedScreenHeight;

	const bool hires = srcWidth > SNES_WIDTH;
	const bool interlaced = srcHeight > SNES_HEIGHT_EXTENDED;

	const int srcPitch = GFX.Pitch >> 1;
	const int srcStep = (interlaced ? srcPitch << 1 : srcPitch) - (hires ? avi_width << 1 : avi_width);
	const int dstStep = avi_pitch + avi_width * 3;

#ifdef LSB_FIRST
	const bool order_is_rgb = (GUI.RedShift < GUI.BlueShift);
#else
	const bool order_is_rgb = (GUI.RedShift > GUI.BlueShift);
#endif

	const int image_offset = (avi_height - (interlaced?srcHeight>>1:srcHeight)) * avi_pitch;
	uint16 *s = GFX.Screen;
	uint8  *d = avi_buffer + (avi_height - 1) * avi_pitch;

	for(int y = 0; y < avi_height; y++)
	{
		for(int x = 0; x < avi_width; x++)
		{
			uint32 pixel;
			if(hires && interlaced) {
				uint16 *s2 = s + srcPitch;
				pixel = Interp(Interp(*s,*(s+1)),Interp(*s2,*(s2+1)));
				s+=2;
			} else if(interlaced) {
				uint16 *s2 = s + srcPitch;
				pixel = Interp(*s,*s2);
				s++;
			} else if(hires) {
				pixel = Interp(*s,*(s+1));
				s+=2;
			} else {
				pixel = *s;
				s++;
			}

			if(order_is_rgb)
			{
				// Order is RGB
				*(d + 0) = (pixel >> (11 - 3)) & 0xf8;
				*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
				*(d + 2) = (pixel & 0x1f) << 3;
				d += 3;
			}
			else
			{
				// Order is BGR
				*(d + 0) = (pixel & 0x1f) << 3;
				*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
				*(d + 2) = (pixel >> (11 - 3)) & 0xf8;
				d += 3;
			}
		}
		s += srcStep;
		d -= dstStep;
	}

	// black out what we might have missed
	if(image_offset > 0)
		memset(avi_buffer, 0, image_offset);
}

// Src: GFX.Screen (variable size) 16bpp top-down
// Dst: avi_buffer 512x2H    (2x2) 24bpp bottom-up
void BuildAVIVideoFrame2X (void)
{
	const int srcWidth = IPPU.RenderedScreenWidth;
	const int srcHeight = IPPU.RenderedScreenHeight;

	const bool hires = srcWidth > SNES_WIDTH;
	const bool interlaced = srcHeight > SNES_HEIGHT_EXTENDED;

	const int srcPitch = GFX.Pitch >> 1;
	const int srcStep = (interlaced ? srcPitch << 1 : srcPitch) - (hires ? avi_width : avi_width >> 1);
	const int dstStep = (avi_pitch << 1) + avi_width * 3;

#ifdef LSB_FIRST
	const bool order_is_rgb = (GUI.RedShift < GUI.BlueShift);
#else
	const bool order_is_rgb = (GUI.RedShift > GUI.BlueShift);
#endif

	const int image_offset = (avi_height - (interlaced?srcHeight:srcHeight<<1)) * avi_pitch;
	uint16 *s = GFX.Screen;
	uint8  *d = avi_buffer + (avi_height - 1) * avi_pitch;
	uint8  *d2 = d - avi_pitch;

	for(int y = 0; y < avi_height >> 1; y++)
	{
		for(int x = 0; x < avi_width >> 1; x++)
		{
			uint32 pixel, pixel2, pixel3, pixel4;
			if(hires && interlaced) {
				uint16 *s2 = s + srcPitch;
				pixel = *s;
				pixel2 = *(s+1);
				pixel3 = *s2;
				pixel4 = *(s2+1);
				s+=2;
			} else if(interlaced) {
				uint16 *s2 = s + srcPitch;
				pixel = pixel2 = *s;
				pixel3 = pixel4 = *s2;
				s++;
			} else if(hires) {
				pixel = pixel3 = *s;
				pixel2 = pixel4 = *(s+1);
				s+=2;
			} else {
				pixel = pixel2 = pixel3 = pixel4 = *s;
				s++;
			}

			if(order_is_rgb)
			{
				// Order is RGB
				*(d + 0) = (pixel >> (11 - 3)) & 0xf8;
				*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
				*(d + 2) = (pixel & 0x1f) << 3;

				*(d + 0) = (pixel2 >> (11 - 3)) & 0xf8;
				*(d + 1) = (pixel2 >> (6 - 3)) & 0xf8;
				*(d + 2) = (pixel2 & 0x1f) << 3;
				d += 6;

				*(d2 + 0) = (pixel3 >> (11 - 3)) & 0xf8;
				*(d2 + 1) = (pixel3 >> (6 - 3)) & 0xf8;
				*(d2 + 2) = (pixel3 & 0x1f) << 3;

				*(d2 + 0) = (pixel4 >> (11 - 3)) & 0xf8;
				*(d2 + 1) = (pixel4 >> (6 - 3)) & 0xf8;
				*(d2 + 2) = (pixel4 & 0x1f) << 3;
				d2 += 6;
			}
			else
			{
				// Order is BGR
				*(d + 0) = (pixel & 0x1f) << 3;
				*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
				*(d + 2) = (pixel >> (11 - 3)) & 0xf8;

				*(d + 3) = (pixel2 & 0x1f) << 3;
				*(d + 4) = (pixel2 >> (6 - 3)) & 0xf8;
				*(d + 5) = (pixel2 >> (11 - 3)) & 0xf8;
				d += 6;

				*(d2 + 0) = (pixel3 & 0x1f) << 3;
				*(d2 + 1) = (pixel3 >> (6 - 3)) & 0xf8;
				*(d2 + 2) = (pixel3 >> (11 - 3)) & 0xf8;

				*(d2 + 3) = (pixel4 & 0x1f) << 3;
				*(d2 + 4) = (pixel4 >> (6 - 3)) & 0xf8;
				*(d2 + 5) = (pixel4 >> (11 - 3)) & 0xf8;
				d2 += 6;
			}
		}
		s += srcStep;
		d -= dstStep;
		d2 -= dstStep;
	}

	// black out what we might have missed
	if(image_offset > 0)
		memset(avi_buffer, 0, image_offset);
}

void DoAVIOpen(const TCHAR* filename)
{
	// close current instance
	if(GUI.AVIOut)
	{
		AVIClose(&GUI.AVIOut);
		GUI.AVIOut = NULL;
	}
	
	pre_avi_soundsync = Settings.SoundSync;
	pre_avi_soundinputrate = Settings.SoundInputRate;
	Settings.SoundSync = false;
	Settings.SoundInputRate = 32000;
	ReInitSound();
	CloseSoundDevice();

	// create new writer
	AVICreate(&GUI.AVIOut);

	int framerate = Memory.ROMFramesPerSecond;
	int frameskip = Settings.SkipFrames;
	if(frameskip == AUTO_FRAMERATE)
		frameskip = 1;
	else
		frameskip++;

	AVISetFramerate(framerate, frameskip, GUI.AVIOut);

	avi_width = SNES_WIDTH;
	avi_height = Settings.ShowOverscan ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT;
	avi_skip_frames = Settings.SkipFrames;

	if(GUI.AVIHiRes) {
		avi_width *= 2;
		avi_height *= 2;
	}

	if(avi_height % 2 != 0) // most codecs can't handle odd-height images
		avi_height++;

	avi_pitch = ((avi_width * 3) + 3) & ~3; //((avi_width * 24 + 31) / 8) & ~3;
	avi_image_size = avi_pitch * avi_height;

	BITMAPINFOHEADER bi;
	memset(&bi, 0, sizeof(bi));
	bi.biSize = 0x28;
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	bi.biWidth = avi_width;
	bi.biHeight = avi_height;
	bi.biSizeImage = avi_image_size;

	AVISetVideoFormat(&bi, GUI.AVIOut);

	WAVEFORMATEX wfx;

	wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = Settings.SoundPlaybackRate;
    wfx.nBlockAlign = 2 * 2;
    wfx.wBitsPerSample = 16;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

	if(!GUI.Mute)
	{
		AVISetSoundFormat(&wfx, GUI.AVIOut);
	}

	if(!AVIBegin(filename, GUI.AVIOut))
	{
		DoAVIClose(2);
		GUI.AVIOut = NULL;
		return;
	}

	avi_sound_samples_per_update = (double) (wfx.nSamplesPerSec * frameskip) / framerate;
	avi_sound_bytes_per_sample = wfx.nBlockAlign;
	avi_sound_samples_error = 0;

	// init buffers
	avi_buffer = new uint8[avi_image_size];
	avi_sound_buffer = new uint8[(int) ceil(avi_sound_samples_per_update) * avi_sound_bytes_per_sample];
}

void DoAVIClose(int reason)
{
	if(!GUI.AVIOut)
	{
		return;
	}

	AVIClose(&GUI.AVIOut);
	GUI.AVIOut = NULL;

	delete [] avi_buffer;
	delete [] avi_sound_buffer;

	avi_buffer = NULL;
	avi_sound_buffer = NULL;

	Settings.SoundSync = pre_avi_soundsync;
	Settings.SoundInputRate = pre_avi_soundinputrate;
	ReInitSound();

	switch(reason)
	{
	case 1:
		// emu settings changed
		S9xMessage(S9X_INFO, S9X_AVI_INFO, AVI_CONFIGURATION_CHANGED);
		break;
	case 2:
		// create AVI failed
		S9xMessage(S9X_INFO, S9X_AVI_INFO, AVI_CREATION_FAILED);
		break;
	default:
		// print nothing
		break;
	}
}

void DoAVIVideoFrame()
{
	static uint32 lastFrameCount=0;
	if(!GUI.AVIOut || !avi_buffer || (IPPU.FrameCount==lastFrameCount))
	{
		return;
	}
	lastFrameCount=IPPU.FrameCount;

	// check configuration
	const WAVEFORMATEX* pwfex = NULL;
	WAVEFORMATEX wfx;
	wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = Settings.SoundPlaybackRate;
    wfx.nBlockAlign = 2 * 2;
    wfx.wBitsPerSample = 16;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;
	if(avi_skip_frames != Settings.SkipFrames ||
		(AVIGetSoundFormat(GUI.AVIOut, &pwfex) && memcmp(pwfex, &wfx, sizeof(WAVEFORMATEX))))
	{
		DoAVIClose(1);
		return;
	}

	if(GUI.AVIHiRes)
		BuildAVIVideoFrame2X();
	else
		BuildAVIVideoFrame1X();

	// write to AVI
	AVIAddVideoFrame(avi_buffer, GUI.AVIOut);

	// generate sound
	if(pwfex)
	{
		const int stereo_multiplier = 2;

		avi_sound_samples_error += avi_sound_samples_per_update;
		int samples = (int) avi_sound_samples_error;
		avi_sound_samples_error -= samples;

		S9xMixSamples(avi_sound_buffer, samples*stereo_multiplier);

		AVIAddSoundSamples(avi_sound_buffer, samples, GUI.AVIOut);
	}
}
