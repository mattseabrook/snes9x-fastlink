/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef SetFlags
#undef SetFlags
#endif
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <string>
#include "IS9xSoundOutput.h"

class CWasapi : public IS9xSoundOutput
{
  private:
    IMMDevice *device;
    IAudioClient *audioClient;
    IAudioRenderClient *renderClient;
    HANDLE sampleEvent;
    bool initDone;
    UINT32 bufferFrames;
    UINT32 bytesPerFrame;
    double volume;

    bool InitClient(bool exclusive);
    void ReleaseClient();

    void ProcessSound(void);

  public:
    CWasapi(void);
    ~CWasapi(void);

    bool InitSoundOutput(void);
    void DeInitSoundOutput(void);
    bool SetupSound(void);
    void SetVolume(double volume);
    HANDLE GetProcessEvent() override { return sampleEvent; }
    std::vector<std::wstring> GetDeviceList();
    int FindDeviceIndex(TCHAR *audio_device);
};
