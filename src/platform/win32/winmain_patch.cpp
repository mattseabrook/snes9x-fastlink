int WINAPI WinMain(
 HINSTANCE hInstance,
 HINSTANCE hPrevInstance,
 LPSTR lpCmdLine,
 int nCmdShow)
{
Settings.StopEmulation = TRUE;
SetCurrentDirectory(S9xGetDirectoryT(DEFAULT_DIR));

WinRegisterConfigItems();
ConfigFile::SetAlphaSort(false);
ConfigFile::SetTimeSort(false);

    const TCHAR *rom_filename = WinParseCommandLineAndLoadConfigFile(GetCommandLine());
    WinSaveConfigFile();
WinLockConfigFile();
LoadExts();

    ControllerOptionsFromControllers();
    ChangeInputDevice();
    WinInit(hInstance);

if(GUI.HideMenu) SetMenu(GUI.hWnd, nullptr);

    DEV_BROADCAST_DEVICEINTERFACE notificationFilter{};
    notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    RegisterDeviceNotification(GUI.hWnd, &notificationFilter, DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);

InitRenderFilters();
    GUI.ControlForced = 0xff;
    Settings.Rewinding = false;

    S9xSetRecentGames();
RestoreMainWinPos();

void InitSnes9x(void);
InitSnes9x();

if(GUI.FullScreen) {
 = false;
();
}

    TIMECAPS tc;
DWORD wSoundTimerRes = (timeGetDevCaps(&tc, sizeof(TIMECAPS)) == TIMERR_NOERROR) ?
t>(tc.wPeriodMin, 1, tc.wPeriodMax) : 5;
timeBeginPeriod(wSoundTimerRes);

    Settings.StopEmulation = TRUE;

if (GUI.JoystickHotkeys || GUI.BackgroundInput)
Timer = nullptr;

#ifdef NETPLAY_SUPPORT
    GUI.ServerTimerSemaphore = CreateSemaphore(nullptr, 0, 10, nullptr);
#endif

if (rom_filename) {
gs.Multi) {
rom_filename); 
ame, multiRomB);
{
ame);
gs.InitialSnapshotFilename) S9xUnfreezeGame(Settings.InitialSnapshotFilename);
}

S9xUnmapAllControls();
S9xSetupDefaultKeymap();

std::thread MemoryServeThread;k
bool isMemServeRunning = false;
bool isMemShareRunning = false;

DWORD lastTime = timeGetTime();
    MSG msg;

    for (;;) [[likely]]
    {
sureInputDisplayUpdated();

        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
                goto loop_exit;

            if (!TranslateAccelerator(GUI.hWnd, GUI.Accelerators, &msg))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

st bool isPaused = Settings.StopEmulation || (Settings.Paused && !Settings.FrameAdvance) || Settings.ForcedPause;
dMute(GUI.Mute || (isPaused && (!Settings.FrameAdvance || GUI.FAMute)));

[[unlikely]]
tinue;
ETPLAY_SUPPORT
        if (!Settings.NetPlay || !NetPlay.PendingWait4Sync ||
            WaitForSingleObject(GUI.ClientSemaphore, 100) != WAIT_TIMEOUT)
        {
            if (NetPlay.PendingWait4Sync)
            {
                NetPlay.PendingWait4Sync = FALSE;
                NetPlay.FrameCount++;
                S9xNPStepJoypadHistory();
            }
#endif
 watches snapshot 
st auto& w : watches) {
) [[unlikely]] {
st int address = w.address - 0x7E0000;
st auto source = (address < 0x20000) ? Memory.RAM + address :
                  (address < 0x30000) ? Memory.SRAM + address - 0x20000 :
                  Memory.FillRAM + address - 0x30000;
+ address, source, w.size);
D) [[unlikely]] {
st auto now = timeGetTime(); now - lastTime >= 100) {
D, WM_COMMAND, IDC_REFRESHLIST, 0);
now;
gs.FrameAdvance) {
foStringTimeout = std::min<int>(GFX.InfoStringTimeout, 4);
gs.FrameAdvance = GUI.FASkipsNonInput;
ceJustPressed) GUI.FrameAdvanceJustPressed--;

dBufferSize 
#ifdef NETPLAY_SUPPORT
gs.NetPlay 
#endif
(Settings.Rewinding) Settings.Rewinding = stateMan.pop();
(IPPU.TotalEmulatedFrames % GUI.rewindGranularity == 0) stateMan.push();
k Lifecycle
gs.MemoryServe != isMemServeRunning) [[unlikely]] {
ning = Settings.MemoryServe;
ning) {
= std::thread(MemServe);
{
ServeThread.joinable()) MemoryServeThread.join();
gs.MemoryMapServe != isMemShareRunning) [[unlikely]] {
ning = Settings.MemoryMapServe;
ning) isMemShareRunning = MemShareStart();
put();
Loop();

ning) {
TEGER emuCounter;
ceCounter(&emuCounter);
sizeof(Memory.RAM), static_cast<uint64_t>(GUI.FrameCount + 1), emuCounter.QuadPart);
t++;
&& --GUI.CursorTimer == 0) [[unlikely]] {
trollerOption != SNES_SUPERSCOPE && GUI.ControllerOption != SNES_JUSTIFIER && GUI.ControllerOption != SNES_JUSTIFIER_2 && GUI.ControllerOption != SNES_MACSRIFLE)
ullptr);
ETPLAY_SUPPORT
        }
#endif
        if (CPU.Flags & DEBUG_MODE_FLAG) [[unlikely]]
        {
            Settings.Paused = true;
            Settings.FrameAdvance = false;
            CPU.Flags &= ~DEBUG_MODE_FLAG;
        }
    }

loop_exit:
if (isMemServeRunning) { MemServeStop(); if (MemoryServeThread.joinable()) MemoryServeThread.join(); }
if (isMemShareRunning) MemShareStop();

VisualizationShutdown();
Settings.StopEmulation = TRUE;

CloseSoundDevice();
StopHotkeyTimer();
    timeEndPeriod(wSoundTimerRes);

    if (!Settings.StopEmulation) {
        Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR).c_str());
        S9xSaveCheatFile(S9xGetFilename(".cht", CHEAT_DIR).c_str());
    }

DoAVIClose(0);
S9xMovieShutdown(); 

WinUnlockConfigFile();
    WinSaveConfigFile();
WinCleanupConfigData();
Memory.Deinit();
ClearExts();
DeInitRenderFilters();
S9xGraphicsDeinit();
S9xDeinitAPU();
WinDeleteRecentGamesList();
DeinitS9x();

return msg.wParam;
}
