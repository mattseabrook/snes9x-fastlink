/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

/* win32_display.cpp
	Contains most of the display related functions of the win32 port.
	Delegates relevant function calls to the currently selected IS9xDisplayOutput object.
*/

#include "core/util/snes9x.h"
#include "core/ppu/ppu.h"
#include "core/video-common/font.h"
#include "wsnes9x.h"
#include "win32_display.h"
#include <immintrin.h> // SIMD AVX2 support
#include <intrin.h>
#include "CDirect3D.h"
#include "CVulkan.h"
#include "IS9xDisplayOutput.h"

#include "filter/hq2x.h"
#include "filter/2xsai.h"
#include "core/apu/apu.h"

#include <atomic>
#include <mutex>

// available display output methods
CDirect3D Direct3D;
CVulkan VulkanDriver;
SSurface Src = {0};
extern BYTE *ScreenBufferBlend;

typedef HRESULT (*DWMFLUSHPROC)();
typedef HRESULT (*DWMISCOMPOSITIONENABLEDPROC)(BOOL *);
DWMFLUSHPROC DwmFlushProc = NULL;
DWMISCOMPOSITIONENABLEDPROC DwmIsCompositionEnabledProc = NULL;
static std::atomic<int64_t> g_throttle_carry_debt_us { 0 };

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Interface used to access the display output
IS9xDisplayOutput *S9xDisplayOutput=&Direct3D;

struct S9xFrameHandoffState
{
	std::mutex mutex;
	std::vector<BYTE> framePixels[2];
	int frameWidth[2] = {0, 0};
	int frameHeight[2] = {0, 0};
	int framePitch[2] = {0, 0};
	int latestIndex = -1;
	int renderIndex = -1;
	HANDLE frameReadyEvent = NULL;
	uint64_t latestSerial = 0;
	uint64_t renderedSerial = 0;
	bool latestOnly = true;
	bool enabled = false;
};

static S9xFrameHandoffState g_frame_handoff;
static BYTE *g_src_base_surface = NULL;

struct S9xLatencyTraceState
{
	std::atomic<LONGLONG> inputSampleQpc{0};
	std::atomic<LONGLONG> emuStartQpc{0};
	std::atomic<LONGLONG> emuEndQpc{0};
	std::atomic<LONGLONG> framePublishQpc{0};
	std::atomic<uint32_t> presentCounter{0};
	std::atomic<bool> enabled{false};
	LARGE_INTEGER qpcFreq = {};
	std::mutex logMutex;
};

static S9xLatencyTraceState g_latency_trace;

#ifndef max
#define max(a,b)            (((a) > (b)) ? (a) : (b))
#endif
#ifndef min
#define min(a,b)            (((a) < (b)) ? (a) : (b))
#endif

bool8 S9xDeinitUpdate (int, int);
void DoAVIVideoFrame();

static LONGLONG WinLatencyNowQpc()
{
	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

static double WinLatencyQpcToMs(LONGLONG delta)
{
	if (g_latency_trace.qpcFreq.QuadPart <= 0)
		QueryPerformanceFrequency(&g_latency_trace.qpcFreq);

	if (g_latency_trace.qpcFreq.QuadPart <= 0)
		return 0.0;

	return (double)delta * 1000.0 / (double)g_latency_trace.qpcFreq.QuadPart;
}

static void WinLatencyLogLine(const char *line)
{
	if (!g_latency_trace.enabled.load(std::memory_order_relaxed))
		return;

	OutputDebugStringA(line);

	char module_path[MAX_PATH] = {};
	if (GetModuleFileNameA(NULL, module_path, MAX_PATH) <= 0)
		return;

	char *slash = strrchr(module_path, '\\');
	if (!slash)
		return;
	slash[1] = '\0';
	strncat(module_path, "snes9x_latency.log", MAX_PATH - strlen(module_path) - 1);

	std::lock_guard<std::mutex> lock(g_latency_trace.logMutex);
	HANDLE h = CreateFileA(module_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;

	DWORD written = 0;
	WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
	CloseHandle(h);
}

static void WinLatencyMarkFramePublished()
{
	g_latency_trace.framePublishQpc.store(WinLatencyNowQpc(), std::memory_order_relaxed);
}

void WinLatencyMarkInputSample()
{
	g_latency_trace.inputSampleQpc.store(WinLatencyNowQpc(), std::memory_order_relaxed);
}

void WinLatencyMarkEmuStepStart()
{
	g_latency_trace.emuStartQpc.store(WinLatencyNowQpc(), std::memory_order_relaxed);
}

void WinLatencyMarkEmuStepEnd()
{
	g_latency_trace.emuEndQpc.store(WinLatencyNowQpc(), std::memory_order_relaxed);
}

void WinLatencyMarkPresentSubmit()
{
	if (!g_latency_trace.enabled.load(std::memory_order_relaxed))
		return;

	const LONGLONG presentQpc = WinLatencyNowQpc();
	const LONGLONG inputQpc = g_latency_trace.inputSampleQpc.load(std::memory_order_relaxed);
	const LONGLONG emuStartQpc = g_latency_trace.emuStartQpc.load(std::memory_order_relaxed);
	const LONGLONG emuEndQpc = g_latency_trace.emuEndQpc.load(std::memory_order_relaxed);
	const LONGLONG publishQpc = g_latency_trace.framePublishQpc.load(std::memory_order_relaxed);

	uint32_t count = g_latency_trace.presentCounter.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((count & 0x3f) != 0)
		return;

	if (inputQpc <= 0 || emuStartQpc < inputQpc || emuEndQpc < emuStartQpc || publishQpc < emuEndQpc || presentQpc < publishQpc)
		return;

	char buf[256];
	_snprintf(buf, sizeof(buf),
		"[SNES9X-LAT] input->emu_start=%.3fms emu=%.3fms emu_end->publish=%.3fms publish->present_submit=%.3fms total_input->present_submit=%.3fms\n",
		WinLatencyQpcToMs(emuStartQpc - inputQpc),
		WinLatencyQpcToMs(emuEndQpc - emuStartQpc),
		WinLatencyQpcToMs(publishQpc - emuEndQpc),
		WinLatencyQpcToMs(presentQpc - publishQpc),
		WinLatencyQpcToMs(presentQpc - inputQpc));
	buf[sizeof(buf) - 1] = '\0';
	WinLatencyLogLine(buf);
}

void WinEnableFrameHandoff(bool enabled)
{
	HANDLE closeEvent = NULL;
	if (enabled)
	{
		char env[8] = {};
		DWORD len = GetEnvironmentVariableA("SNES9X_LATENCY_LOG", env, sizeof(env));
		bool traceEnabled = (len > 0 && env[0] == '1');
		g_latency_trace.enabled.store(traceEnabled, std::memory_order_relaxed);
	}
	else
	{
		g_latency_trace.enabled.store(false, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
		if (enabled && !g_frame_handoff.frameReadyEvent)
			g_frame_handoff.frameReadyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

		if (!enabled && g_frame_handoff.frameReadyEvent)
		{
			closeEvent = g_frame_handoff.frameReadyEvent;
			g_frame_handoff.frameReadyEvent = NULL;
		}

		g_frame_handoff.enabled = enabled;
		g_frame_handoff.latestOnly = GUI.LowLatencyFrameHandoff;
		if (!enabled)
		{
			g_frame_handoff.framePixels[0].clear();
			g_frame_handoff.framePixels[1].clear();
			g_frame_handoff.frameWidth[0] = g_frame_handoff.frameWidth[1] = 0;
			g_frame_handoff.frameHeight[0] = g_frame_handoff.frameHeight[1] = 0;
			g_frame_handoff.framePitch[0] = g_frame_handoff.framePitch[1] = 0;
			g_frame_handoff.latestIndex = -1;
			g_frame_handoff.renderIndex = -1;
			g_frame_handoff.latestSerial = 0;
			g_frame_handoff.renderedSerial = 0;
			g_src_base_surface = NULL;
			g_latency_trace.inputSampleQpc.store(0, std::memory_order_relaxed);
			g_latency_trace.emuStartQpc.store(0, std::memory_order_relaxed);
			g_latency_trace.emuEndQpc.store(0, std::memory_order_relaxed);
			g_latency_trace.framePublishQpc.store(0, std::memory_order_relaxed);
			g_latency_trace.presentCounter.store(0, std::memory_order_relaxed);
		}
	}

	if (closeEvent)
		CloseHandle(closeEvent);
}

void WinClearFrameHandoff()
{
	std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
	g_frame_handoff.framePixels[0].clear();
	g_frame_handoff.framePixels[1].clear();
	g_frame_handoff.frameWidth[0] = g_frame_handoff.frameWidth[1] = 0;
	g_frame_handoff.frameHeight[0] = g_frame_handoff.frameHeight[1] = 0;
	g_frame_handoff.framePitch[0] = g_frame_handoff.framePitch[1] = 0;
	g_frame_handoff.latestIndex = -1;
	g_frame_handoff.renderIndex = -1;
	g_frame_handoff.latestSerial = 0;
	g_frame_handoff.renderedSerial = 0;
	if (g_frame_handoff.frameReadyEvent)
		ResetEvent(g_frame_handoff.frameReadyEvent);
	g_src_base_surface = NULL;
	g_latency_trace.inputSampleQpc.store(0, std::memory_order_relaxed);
	g_latency_trace.emuStartQpc.store(0, std::memory_order_relaxed);
	g_latency_trace.emuEndQpc.store(0, std::memory_order_relaxed);
	g_latency_trace.framePublishQpc.store(0, std::memory_order_relaxed);
	g_latency_trace.presentCounter.store(0, std::memory_order_relaxed);
}

HANDLE WinGetFrameReadyEvent()
{
	std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
	return g_frame_handoff.frameReadyEvent;
}

static void WinPublishFrame(const BYTE *surface, int width, int height, int pitch)
{
	if (!surface || width <= 0 || height <= 0 || pitch <= 0)
		return;

	int writeIndex = 0;
	BYTE *dst = NULL;
	HANDLE frameReadyEvent = NULL;
	const int packedPitch = width * 2;
	if (packedPitch <= 0)
		return;
	const size_t copySize = static_cast<size_t>(height) * static_cast<size_t>(packedPitch);

	{
		std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
		if (!g_frame_handoff.enabled)
			return;

		bool preferLatestOnly = g_frame_handoff.latestOnly;
		HWND activeWindow = GetActiveWindow();
		if (preferLatestOnly && activeWindow && activeWindow != GUI.hWnd)
			preferLatestOnly = false;

		if (preferLatestOnly)
		{
			if (g_frame_handoff.renderIndex == 0)
				writeIndex = 1;
			else if (g_frame_handoff.renderIndex == 1)
				writeIndex = 0;
			else
				writeIndex = (g_frame_handoff.latestIndex == 0) ? 1 : 0;
		}
		else
		{
			if (g_frame_handoff.renderIndex == 0)
				writeIndex = 1;
			else if (g_frame_handoff.renderIndex == 1)
				writeIndex = 0;
			else
				writeIndex = (g_frame_handoff.latestIndex == 0) ? 1 : 0;
		}

		if (g_frame_handoff.framePixels[writeIndex].size() != copySize)
			g_frame_handoff.framePixels[writeIndex].resize(copySize);

		dst = g_frame_handoff.framePixels[writeIndex].data();
		frameReadyEvent = g_frame_handoff.frameReadyEvent;
	}

	if (pitch == packedPitch)
	{
		memcpy(dst, surface, copySize);
	}
	else
	{
		for (int y = 0; y < height; y++)
			memcpy(dst + (size_t)y * packedPitch, surface + (size_t)y * pitch, packedPitch);
	}

	{
		std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
		if (!g_frame_handoff.enabled)
			return;
		g_frame_handoff.frameWidth[writeIndex] = width;
		g_frame_handoff.frameHeight[writeIndex] = height;
		g_frame_handoff.framePitch[writeIndex] = packedPitch;
		g_frame_handoff.latestIndex = writeIndex;
		g_frame_handoff.latestSerial++;
	}

	WinLatencyMarkFramePublished();

	if (frameReadyEvent)
		SetEvent(frameReadyEvent);
}

bool WinConsumeFrameAndRender()
{
	int consumeIndex = -1;
	int width = 0;
	int height = 0;
	int pitch = 0;
	BYTE *surface = NULL;

	{
		std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
		if (!g_frame_handoff.enabled || g_frame_handoff.latestSerial == g_frame_handoff.renderedSerial || g_frame_handoff.latestIndex < 0)
			return false;

		consumeIndex = g_frame_handoff.latestIndex;
		width = g_frame_handoff.frameWidth[consumeIndex];
		height = g_frame_handoff.frameHeight[consumeIndex];
		pitch = g_frame_handoff.framePitch[consumeIndex];
		surface = g_frame_handoff.framePixels[consumeIndex].data();
		g_frame_handoff.renderIndex = consumeIndex;
		g_frame_handoff.renderedSerial = g_frame_handoff.latestSerial;
	}

	if (width <= 0 || height <= 0 || pitch <= 0 || !surface)
		return false;

	Src.Width = width;
	Src.Height = height;
	Src.Pitch = pitch;
	g_src_base_surface = surface;
	Src.Surface = surface;

	WinRefreshDisplay();

	{
		std::lock_guard<std::mutex> lock(g_frame_handoff.mutex);
		if (g_frame_handoff.renderIndex == consumeIndex)
			g_frame_handoff.renderIndex = -1;
	}

	return true;
}

// cut off both top and bottom if overscan is disabled and game outputs extended height,
// center image if overscan is enabled and game outputs regular height
static void CheckOverscanOffset()
{
	int lines_to_skip = 0;
	if (!Settings.ShowOverscan)
	{
		if (Src.Height == SNES_HEIGHT_EXTENDED)
			lines_to_skip = 7;
		else if (Src.Height == SNES_HEIGHT_EXTENDED << 1)
			lines_to_skip = 14;
	}
	else
	{
		if (Src.Height == SNES_HEIGHT)
			lines_to_skip = -8;
		else if (Src.Height == SNES_HEIGHT << 1)
			lines_to_skip = -16;
	}

	BYTE *base = g_src_base_surface ? g_src_base_surface : (BYTE *)GFX.Screen;
	int pitch = Src.Pitch ? Src.Pitch : (int)GFX.RealPPL * 2;
	Src.Surface = base + lines_to_skip * pitch;
}

/*  WinRefreshDisplay
repeats the last rendered frame
*/
void WinRefreshDisplay(void)
{
	if(!Src.Width)
		return;

	CheckOverscanOffset();

	static RenderFilter lastScale = FILTER_NONE;
	static RenderFilter lastScaleHiRes = FILTER_NONE;
	if (lastScale != GUI.Scale || lastScaleHiRes != GUI.ScaleHiRes)
	{
		SelectRenderMethod();
		lastScale = GUI.Scale;
		lastScaleHiRes = GUI.ScaleHiRes;
	}

	S9xDisplayOutput->Render(Src);
	GUI.FlipCounter++;
}

void WinChangeWindowSize(unsigned int newWidth, unsigned int newHeight)
{
	S9xDisplayOutput->ChangeRenderSize(newWidth,newHeight);
}

/*  WinDisplayReset
initializes the currently selected display output and
reinitializes the core graphics rendering
-----
returns true if successful, false otherwise
*/
bool WinDisplayReset(void)
{
	const TCHAR *oldDriverName = (GUI.outputMethod == VULKAN) ? TEXT("Vulkan") : TEXT("Direct3D11");
	S9xDisplayOutput->DeInitialize();

	switch(GUI.outputMethod) {
		default:
		case DIRECT3D:
			GUI.outputMethod = DIRECT3D;
			S9xDisplayOutput = &Direct3D;
			break;
		case VULKAN:
			S9xDisplayOutput = &VulkanDriver;
			break;
	}

	bool initialized = S9xDisplayOutput->Initialize(GUI.hWnd);

	if (!initialized) {
		S9xDisplayOutput->DeInitialize();

		GUI.outputMethod = DIRECT3D;
		S9xDisplayOutput = &Direct3D;

		const TCHAR *newDriverName = TEXT("Direct3D11");
		TCHAR msg[512];
		_stprintf(msg, TEXT("Couldn't load selected driver: %s. Trying %s."), oldDriverName, newDriverName);
		MessageBox(GUI.hWnd, msg, TEXT("Snes9x Display Driver"), MB_OK | MB_ICONERROR);

		initialized = S9xDisplayOutput->Initialize(GUI.hWnd);
	}

	if (initialized) {
		S9xGraphicsDeinit();
		S9xSetWinPixelFormat();
		S9xGraphicsInit();

		if (GUI.DWMSync)
		{
			HMODULE dwmlib = LoadLibrary(TEXT("dwmapi"));
            DwmFlushProc = (DWMFLUSHPROC)GetProcAddress(dwmlib, "DwmFlush");
            DwmIsCompositionEnabledProc = (DWMISCOMPOSITIONENABLEDPROC)GetProcAddress(dwmlib, "DwmIsCompositionEnabled");

            if (!DwmFlushProc || !DwmIsCompositionEnabledProc)
            {
                MessageBox(GUI.hWnd, TEXT("Couldn't load DWM functions. DWM Sync is disabled."), TEXT("Warning"), MB_OK | MB_ICONWARNING);
                GUI.DWMSync = false;
            }
        }

		return true;
	} else {
		MessageBox (GUI.hWnd, Languages[ GUI.Language].errInitDD, TEXT("Snes9x - Display Failure"), MB_OK | MB_ICONSTOP);
		return false;
	}
}

void WinDisplayApplyChanges()
{
	S9xDisplayOutput->ApplyDisplayChanges();
}

RECT CalculateDisplayRect(unsigned int sourceWidth,unsigned int sourceHeight,
						  unsigned int displayWidth,unsigned int displayHeight)
{
	double xFactor;
	double yFactor;
	double minFactor;
	double renderWidthCalc,renderHeightCalc;
	int hExtend = Settings.ShowOverscan ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT;
	double snesAspect = (double)GUI.AspectWidth/hExtend;
	RECT drawRect;

	if(GUI.Stretch) {
		if(GUI.AspectRatio) {

			if (GUI.IntegerScaling && sourceHeight > 0 && sourceHeight <= displayHeight && (int)(sourceHeight * snesAspect) <= displayWidth) {
				int h;
				for (h = sourceHeight * 2; h <= displayHeight && (int)(h * snesAspect) <= displayWidth; h += sourceHeight) {}
				h -= sourceHeight;
				drawRect.right = (LONG)(h * snesAspect);
				drawRect.bottom = h;
			} else {
				//fix for hi-res images with FILTER_NONE
				//where we need to correct the aspect ratio
				renderWidthCalc = (double)sourceWidth;
				renderHeightCalc = (double)sourceHeight;
				if (renderWidthCalc / renderHeightCalc > snesAspect)
					renderWidthCalc = renderHeightCalc * snesAspect;
				else if (renderWidthCalc / renderHeightCalc < snesAspect)
					renderHeightCalc = renderWidthCalc / snesAspect;

				xFactor = (double)displayWidth / renderWidthCalc;
				yFactor = (double)displayHeight / renderHeightCalc;
				minFactor = xFactor < yFactor ? xFactor : yFactor;

				drawRect.right = (LONG)(renderWidthCalc * minFactor);
				drawRect.bottom = (LONG)(renderHeightCalc * minFactor);
			}

			drawRect.left = (displayWidth - drawRect.right) / 2;
			drawRect.top = (displayHeight - drawRect.bottom) / 2;
			drawRect.right += drawRect.left;
			drawRect.bottom += drawRect.top;

		} else {
			drawRect.top = 0;
			drawRect.left = 0;
			drawRect.right = displayWidth;
			drawRect.bottom = displayHeight;
		}
	} else {
		drawRect.left = ((int)(displayWidth) - (int)sourceWidth) / 2;
		drawRect.top = ((int)(displayHeight) - (int)sourceHeight) / 2;
		if(!GUI.AlwaysCenterImage) {
			if(drawRect.left < 0) drawRect.left = 0;
			if(drawRect.top < 0) drawRect.top = 0;
		}
		drawRect.right = drawRect.left + sourceWidth;
		drawRect.bottom = drawRect.top + sourceHeight;
	}
	return drawRect;
}

bool8 S9xInitUpdate (void)
{
	return (TRUE);
}

// only necessary for avi recording
// TODO: check if this can be removed
bool8 S9xContinueUpdate(int Width, int Height)
{
	// called every other frame during interlace

    Src.Width = Width;
	if(Height%SNES_HEIGHT)
	    Src.Height = Height;
	else
	{
		if(Height==SNES_HEIGHT)
			Src.Height=SNES_HEIGHT_EXTENDED;
		else Src.Height=SNES_HEIGHT_EXTENDED<<1;
	}
    Src.Pitch = GFX.Pitch;
    Src.Surface = (BYTE*)GFX.Screen;

	// avi writing
	DoAVIVideoFrame();

	return true;
}

// do the actual rendering of a frame
bool8 S9xDeinitUpdate (int Width, int Height)
{
    Src.Width = Width;
	Src.Height = Height;
    Src.Pitch = GFX.Pitch;

	CheckOverscanOffset();

	// avi writing
	DoAVIVideoFrame();

	// Clear some of the old SNES rendered image
	// when the resolution becomes lower in x or y,
	// otherwise the image processors (filters) might access
	// some of the old rendered data at the edges.
    {
        static int LastWidth = 0;
        static int LastHeight = 0;

        if (Width < LastWidth)
        {
            const int hh = max(LastHeight, Height);
            for (int i = 0; i < hh; i++)
                memset (GFX.Screen + i * (GFX.Pitch>>1) + Width*1, 0, 4);
        }
        if (Height < LastHeight)
		{
            const int ww = max(LastWidth, Width);
            for (int i = Height; i < LastHeight ; i++)
                memset (GFX.Screen + i * (GFX.Pitch>>1), 0, ww * 2);

			// also old clear extended height stuff from drawing surface
			if((int)Src.Height > Height)
				for (int i = Height; i < (int)Src.Height ; i++)
					memset (Src.Surface + i * Src.Pitch, 0, Src.Pitch);
		}
        LastWidth = Width;
        LastHeight = Height;
    }

	WinPublishFrame((BYTE*)GFX.Screen, Width, Height, GFX.Pitch);

	if (!g_frame_handoff.enabled)
		WinRefreshDisplay();

    return (true);
}

/*  S9xSetWinPixelFormat
sets default settings and calls the appropriate display object
*/
void S9xSetWinPixelFormat ()
{
    GUI.NeedDepthConvert = FALSE;
	GUI.DepthConverted = !GUI.NeedDepthConvert;

	S9xBlit2xSaIFilterDeinit();
	S9xBlitHQ2xFilterDeinit();

	S9xDisplayOutput->SetSnes9xColorFormat();
}

char *ReadShaderFileContents(const TCHAR *filename)
{
	HANDLE hFile;
	DWORD size;
	DWORD bytesRead;
	char *contents;

	hFile = CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL,
				OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN , 0);
	if(hFile == INVALID_HANDLE_VALUE){
		return NULL;
	}
	size = GetFileSize(hFile,NULL);
	contents = new char[size+1];
	if(!ReadFile(hFile,contents,size,&bytesRead,NULL)) {
		CloseHandle(hFile);
		delete[] contents;
		return NULL;
	}
	CloseHandle(hFile);
	contents[size] = '\0';
	return contents;

}

void ReduceToPath(TCHAR *filename)
{
    for (int i = lstrlen(filename); i >= 0; i--) {
        if (IS_SLASH(filename[i])) {
            filename[i] = TEXT('\0');
            break;
        }
    }
}

void SaveMainWinPos()
{
	WINDOWPLACEMENT wndPlacement={0};
	wndPlacement.length = sizeof(WINDOWPLACEMENT);
	GetWindowPlacement(GUI.hWnd,&wndPlacement);
	GUI.window_maximized = wndPlacement.showCmd == SW_SHOWMAXIMIZED;
	if(!GUI.FullScreen && !GUI.EmulatedFullscreen)
		GUI.window_size = wndPlacement.rcNormalPosition;
}

void RestoreMainWinPos()
{
	WINDOWPLACEMENT wndPlacement={0};
	wndPlacement.length = sizeof(WINDOWPLACEMENT);
	wndPlacement.showCmd = GUI.window_maximized?SW_SHOWMAXIMIZED:SW_SHOWNORMAL;
	wndPlacement.rcNormalPosition = GUI.window_size;
	SetWindowPlacement(GUI.hWnd,&wndPlacement);
}

/*  ToggleFullScreen
switches between fullscreen and window mode and saves the window position
if EmulateFullscreen is set we simply create a borderless window that spans the screen
*/
void ToggleFullScreen ()
{
    S9xSetPause (PAUSE_TOGGLE_FULL_SCREEN);

    SaveMainWinPos();

	if(GUI.EmulateFullscreen) {
		HMONITOR hm;
		MONITORINFO mi;
		GUI.EmulatedFullscreen = !GUI.EmulatedFullscreen;
		if(GUI.EmulatedFullscreen) {
			if(GetMenu(GUI.hWnd)!=NULL)
				SetMenu(GUI.hWnd,NULL);
			SetWindowLongPtr (GUI.hWnd, GWL_STYLE, WS_POPUP|WS_VISIBLE);
			hm = MonitorFromWindow(GUI.hWnd,MONITOR_DEFAULTTONEAREST);
			mi.cbSize = sizeof(mi);
			GetMonitorInfo(hm,&mi);
			SetWindowPos (GUI.hWnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_DRAWFRAME|SWP_FRAMECHANGED);
		} else {
			SetWindowLongPtr( GUI.hWnd, GWL_STYLE, WS_POPUPWINDOW|WS_CAPTION|
                   WS_THICKFRAME|WS_VISIBLE|WS_MINIMIZEBOX|WS_MAXIMIZEBOX);
			SetMenu(GUI.hWnd,GUI.hMenu);
			SetWindowPos (GUI.hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_DRAWFRAME|SWP_FRAMECHANGED);
			RestoreMainWinPos();
		}
	} else {
		GUI.FullScreen = !GUI.FullScreen;
		if(GUI.FullScreen) {
			if(GetMenu(GUI.hWnd)!=NULL)
				SetMenu(GUI.hWnd,NULL);
			SetWindowLongPtr(GUI.hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
            SetWindowLongPtr(GUI.hWnd, GWL_EXSTYLE, 0);
			if(!S9xDisplayOutput->SetFullscreen(true))
				GUI.FullScreen = false;
            else
                SetWindowPos(GUI.hWnd, HWND_TOPMOST, 0, 0, GUI.FullscreenMode.width, GUI.FullscreenMode.height, SWP_DRAWFRAME | SWP_FRAMECHANGED);
		}
		if(!GUI.FullScreen) {
			SetWindowLongPtr(GUI.hWnd, GWL_STYLE, WS_POPUPWINDOW|WS_CAPTION|
                   WS_THICKFRAME|WS_VISIBLE|WS_MINIMIZEBOX|WS_MAXIMIZEBOX);
            SetWindowLongPtr(GUI.hWnd, GWL_EXSTYLE, WS_EX_ACCEPTFILES | WS_EX_APPWINDOW);
			SetMenu(GUI.hWnd,GUI.hMenu);
			S9xDisplayOutput->SetFullscreen(false);
			SetWindowPos (GUI.hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE|SWP_DRAWFRAME|SWP_FRAMECHANGED);
			RestoreMainWinPos();
		}
		if (GUI.AutomaticInputRate)
		{
			int rate = WinGetAutomaticInputRate();
			if (rate)
			{
				Settings.SoundInputRate = rate;
				S9xUpdateDynamicRate(1, 2);
			}
		}
		S9xGraphicsDeinit();
		S9xSetWinPixelFormat ();
		S9xInitUpdate();
		S9xGraphicsInit();
	}
	IPPU.RenderThisFrame = true;

	S9xClearPause (PAUSE_TOGGLE_FULL_SCREEN);
}

/*  S9xOpenSoundDevice
ennumerates the available display modes of the currently selected output
*/
void WinEnumDisplayModes(std::vector<dMode> *modeVector)
{
	S9xDisplayOutput->EnumModes(modeVector);
}

double WinGetRefreshRate(void)
{
	typedef LONG(WINAPI *PGDCBS) (UINT32, UINT32 *, UINT32 *);
	typedef LONG(WINAPI *PQDC)   (UINT32, UINT32*, DISPLAYCONFIG_PATH_INFO *, UINT32*, DISPLAYCONFIG_MODE_INFO *, DISPLAYCONFIG_TOPOLOGY_ID *);
	static PGDCBS pGDCBS = NULL;
	static PQDC   pQDC   = NULL;
	static int firstrun = 1;

	if (firstrun)
	{
		HMODULE user32 = GetModuleHandleA("user32.dll");
		pQDC = (PQDC) GetProcAddress(user32, "QueryDisplayConfig");
		pGDCBS = (PGDCBS) GetProcAddress(user32, "GetDisplayConfigBufferSizes");
		firstrun = 0;
	}

	double refreshRate = 0.0;

	if (!pGDCBS || !pQDC)
		return refreshRate;

	OSVERSIONINFO ovi;
	DISPLAYCONFIG_TOPOLOGY_ID topologyID;
	unsigned int numPathArrayElements = 0;
	unsigned int numModeInfoArrayElements = 0;
	DISPLAYCONFIG_PATH_INFO * pathInfoArray = NULL;
	DISPLAYCONFIG_MODE_INFO * modeInfoArray = NULL;
	int result = 0;

	ovi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
	if (!GetVersionEx(&ovi))
		return refreshRate;

	if (ovi.dwMajorVersion < 6 || (ovi.dwMajorVersion == 6 && ovi.dwMinorVersion < 1))
		return refreshRate;

	result = pGDCBS(QDC_DATABASE_CURRENT,
		&numPathArrayElements,
		&numModeInfoArrayElements);

	if (result != ERROR_SUCCESS)
		return refreshRate;

	pathInfoArray = (DISPLAYCONFIG_PATH_INFO *)
		malloc(sizeof(DISPLAYCONFIG_PATH_INFO) * numPathArrayElements);
	modeInfoArray = (DISPLAYCONFIG_MODE_INFO *)
		malloc(sizeof(DISPLAYCONFIG_MODE_INFO) * numModeInfoArrayElements);

	result = pQDC(QDC_DATABASE_CURRENT,
		&numPathArrayElements,
		pathInfoArray,
		&numModeInfoArrayElements,
		modeInfoArray,
		&topologyID);

	if (result == ERROR_SUCCESS && numPathArrayElements >= 1)
	{
		refreshRate = (float)pathInfoArray[0].targetInfo.refreshRate.Numerator /
			pathInfoArray[0].targetInfo.refreshRate.Denominator;
	}

	free(modeInfoArray);
	free(pathInfoArray);

	return refreshRate;
}

int WinGetAutomaticInputRate(void)
{
    double refreshRate = WinGetRefreshRate();

    if (refreshRate == 0.0)
        return 0;

    // Try for a close multiple of 60hz
    if (refreshRate > 119.0 && refreshRate < 121.0)
        refreshRate /= 2.0;
    if (refreshRate > 179.0 && refreshRate < 181.0)
        refreshRate /= 3.0;
    if (refreshRate > 239.0 && refreshRate < 241.0)
        refreshRate /= 4.0;

    double newInputRate = refreshRate * 32040.0 / 60.09881389744051 + 0.5;

    if (newInputRate > 32040.0 * 1.05 || newInputRate < 32040.0 * 0.95)
        newInputRate = 0.0;

    return (int)newInputRate;
}

void WinThrottleFramerate()
{
	static HANDLE throttle_timer = nullptr;
	static int64_t PCBase, PCFrameTime, PCFrameTimeNTSC, PCFrameTimePAL, PCStart, PCEnd;
	int64_t carry_debt_us = g_throttle_carry_debt_us.load(std::memory_order_relaxed);

	if (Settings.SkipFrames != AUTO_FRAMERATE)
		return;

	if (Settings.TurboMode || Settings.Paused)
		return;

	// Avoid double-throttling when the render path already blocks (VSync/DWM).
	if (GUI.Vsync || GUI.DWMSync)
		return;

	if (!throttle_timer)
	{
		QueryPerformanceFrequency((LARGE_INTEGER *)&PCBase);

		PCFrameTimeNTSC = (int64_t)(PCBase / NTSC_PROGRESSIVE_FRAME_RATE);
		PCFrameTimePAL = (int64_t)(PCBase / PAL_PROGRESSIVE_FRAME_RATE);

		throttle_timer = CreateWaitableTimerEx(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!throttle_timer)
			throttle_timer = CreateWaitableTimer(NULL, true, NULL);
		QueryPerformanceCounter((LARGE_INTEGER *)&PCStart);
	}

	if (Settings.FrameTime == Settings.FrameTimeNTSC)
		PCFrameTime = PCFrameTimeNTSC;
	else if (Settings.FrameTime == Settings.FrameTimePAL)
		PCFrameTime = PCFrameTimePAL;
	else
		PCFrameTime = (__int64)(PCBase * Settings.FrameTime / 1e6);

	QueryPerformanceCounter((LARGE_INTEGER *)&PCEnd);
	int64_t raw_time_left_us = ((PCFrameTime - (PCEnd - PCStart)) * 1000000) / PCBase;
	int64_t time_left_us = raw_time_left_us - carry_debt_us;

	int64_t PCFrameTime_us = (int64_t)(PCFrameTime * 1000000.0 / PCBase);
	if (time_left_us < -(PCFrameTime_us * 3))
	{
		// Major hitch: reset schedule and drop accumulated debt.
		carry_debt_us = 0;
		g_throttle_carry_debt_us.store(carry_debt_us, std::memory_order_relaxed);
		QueryPerformanceCounter((LARGE_INTEGER *)&PCStart);
		return;
	}
	if (time_left_us > 0)
	{
		if (throttle_timer && time_left_us > 2000)
		{
			LARGE_INTEGER li;
			li.QuadPart = -(time_left_us - 2000) * 10;
			SetWaitableTimer(throttle_timer, &li, 0, NULL, NULL, false);
			WaitForSingleObject(throttle_timer, INFINITE);
		}

		LARGE_INTEGER spin_start;
		QueryPerformanceCounter(&spin_start);
		while (spin_start.QuadPart < PCStart + PCFrameTime)
		{
			QueryPerformanceCounter(&spin_start);
		}
	}
	QueryPerformanceCounter((LARGE_INTEGER *)&PCEnd);
	int64_t elapsed_us = ((PCEnd - PCStart) * 1000000) / PCBase;
	int64_t frame_error_us = elapsed_us - PCFrameTime_us;
	if (frame_error_us > 0)
	{
		carry_debt_us += frame_error_us;
		if (carry_debt_us > PCFrameTime_us * 2)
			carry_debt_us = PCFrameTime_us * 2;
	}
	else if (carry_debt_us > 0)
	{
		carry_debt_us += frame_error_us;
		if (carry_debt_us < 0)
			carry_debt_us = 0;
	}
	g_throttle_carry_debt_us.store(carry_debt_us, std::memory_order_relaxed);

	if ((PCEnd - PCStart) > PCFrameTime + (PCFrameTime / 3))
		PCStart = PCEnd;
	else
		PCStart += PCFrameTime;
}

int64 WinGetThrottleCarryDebtUs()
{
	return g_throttle_carry_debt_us.load(std::memory_order_relaxed);
}

std::vector<ShaderParam> *WinGetShaderParameters()
{
    return S9xDisplayOutput->GetShaderParameters();
}

std::function<void(const char*)> WinGetShaderSaveFunction()
{
    return S9xDisplayOutput->GetShaderParametersSaveFunction();
}

/* Depth conversion functions begin */

void Convert16To24 (SSurface *src, SSurface *dst, RECT *srect)
{
    const int height = srect->bottom - srect->top;
    const int width = srect->right - srect->left;
    const int offset1 = srect->top * src->Pitch + srect->left * 2;
    const int offset2 = ((dst->Height - height) >> 1) * dst->Pitch + ((dst->Width - width) >> 1) * 3;
	const int snesWidth = src->Width;
	const int snesHeight = src->Height;
	const bool doubledX = (snesWidth >= width*2) ? true : false;
	const bool doubledY = (snesHeight >= height*2) ? true : false;

    for (int y = 0; y < height; y++)
    {
		uint16 *s = (uint16 *) ((uint8 *) src->Surface + (doubledY ? y*2 : y) * src->Pitch + offset1);
        uint8 *d = ((uint8 *) dst->Surface + y * dst->Pitch + offset2);

		#define Interp(c1, c2) \
			(c1 == c2) ? c1 : \
			(((((c1 & 0x07E0)      + (c2 & 0x07E0)) >> 1) & 0x07E0) + \
			((((c1 & 0xF81F)      + (c2 & 0xF81F)) >> 1) & 0xF81F))

		if(y >= snesHeight)
		{
			// beyond SNES image - make the row black
			memset(d, 0, width*3);
		}
		else

#ifdef LSB_FIRST
		if (GUI.RedShift < GUI.BlueShift)
#else
		if (GUI.RedShift > GUI.BlueShift)
#endif
        {
			// Order is RGB
			if(!doubledX)
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*(d + 0) = (pixel >> (11 - 3)) & 0xf8;
					*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
					*(d + 2) = (pixel & 0x1f) << 3;
					d += 3;
				}
			}
			else // high-res x, blend:
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = Interp(s[0],s[1]);
					s += 2;
					*(d + 0) = (pixel >> (11 - 3)) & 0xf8;
					*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
					*(d + 2) = (pixel & 0x1f) << 3;
					d += 3;
				}
			}
        }
        else
        {
			// Order is BGR
			if(!doubledX)
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*(d + 0) = (pixel & 0x1f) << 3;
					*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
					*(d + 2) = (pixel >> (11 - 3)) & 0xf8;
					d += 3;
				}
			}
			else // high-res x, blend:
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = Interp(s[0],s[1]);
					s += 2;
					*(d + 0) = (pixel & 0x1f) << 3;
					*(d + 1) = (pixel >> (6 - 3)) & 0xf8;
					*(d + 2) = (pixel >> (11 - 3)) & 0xf8;
					d += 3;
				}
			}
        }
    }
}

void Convert16To32 (SSurface *src, SSurface *dst, RECT *srect)
{
    int height = srect->bottom - srect->top;
    int width = srect->right - srect->left;
    int offset1 = srect->top * src->Pitch + srect->left * 2;
    int offset2 = 0;//((dst->Height - height) >> 1) * dst->Pitch +
        //((dst->Width - width) >> 1) * sizeof (uint32);

    for (int y = 0; y < height; y++)
    {
        uint16 *s = (uint16 *) ((uint8 *) src->Surface + y * src->Pitch + offset1);
        uint32 *d = (uint32 *) ((uint8 *) dst->Surface +
                                         y * dst->Pitch + offset2);
        for (int x = 0; x < width; x++)
        {
            uint32 pixel = *s++;
            *d++ = (((pixel >> 11) & 0x1f) << GUI.RedShift) |
                   (((pixel >> 6) & 0x1f) << GUI.GreenShift) |
                   ((pixel & 0x1f) << GUI.BlueShift);
        }
    }
}

void ConvertDepth (SSurface *src, SSurface *dst, RECT *srect)
{
    // SNES image has been rendered in 16-bit, RGB565 format
    switch (GUI.ScreenDepth)
    {
        case 15: // is this right?
        case 16:
            break;
        case 24:
            Convert16To24 (src, dst, srect);
            break;
        case 32:
            Convert16To32 (src, dst, srect);
            break;
    }
}

/* Depth conversion functions end */
#include "snes9x_imgui.h"
void S9xWinDisplayString(const char *string, int linesFromBottom, int pixelsFromLeft, bool allowWrap, int type)
{
	if (S9xImGuiRunning() && !Settings.AutoDisplayMessages)
	{
		return;
	}
	S9xVariableDisplayString(string, linesFromBottom, pixelsFromLeft, allowWrap, type);
}
