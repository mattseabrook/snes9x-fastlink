/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "CWasapi.h"
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <stdint.h>

#include "core/util/snes9x.h"
#include "../../../src/core/apu/apu.h"
#include "wsnes9x.h"

namespace {

template <typename T>
void SafeRelease(T *&ptr)
{
    if (ptr)
    {
        ptr->Release();
        ptr = nullptr;
    }
}

struct S9xWasapiDiagState
{
    bool enabled = true;
    ULONGLONG lastLogTick = 0;
    uint64_t processCalls = 0;
    uint64_t underrunWrites = 0;
    uint64_t partialWrites = 0;
    uint64_t totalRequestedSamples = 0;
    uint64_t totalMixedSamples = 0;
    uint64_t totalSilentSamples = 0;
};

static S9xWasapiDiagState g_wasapi_diag;

static bool S9xWasapiDiagEnabled()
{
    static int cached = -1;
    if (cached < 0)
    {
        char env[8] = {};
        DWORD len = GetEnvironmentVariableA("SNES9X_AUDIO_DIAG", env, sizeof(env));
        cached = (len > 0 && env[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}

static void S9xWasapiDiagLog(UINT32 bufferFrames, UINT32 padding)
{
    if (!g_wasapi_diag.enabled)
        return;

    ULONGLONG now = GetTickCount64();
    if (now - g_wasapi_diag.lastLogTick < 1000)
        return;

    g_wasapi_diag.lastLogTick = now;

    if (g_wasapi_diag.totalRequestedSamples == 0)
        return;

    double silentPct = 100.0 * (double)g_wasapi_diag.totalSilentSamples / (double)g_wasapi_diag.totalRequestedSamples;
    char buf[256];
    _snprintf(buf, sizeof(buf),
        "[SNES9X-AUDIO] calls=%llu underruns=%llu partial=%llu silent=%.2f%% pad=%u/%u req=%llu mixed=%llu\n",
        (unsigned long long)g_wasapi_diag.processCalls,
        (unsigned long long)g_wasapi_diag.underrunWrites,
        (unsigned long long)g_wasapi_diag.partialWrites,
        silentPct,
        (unsigned)padding,
        (unsigned)bufferFrames,
        (unsigned long long)g_wasapi_diag.totalRequestedSamples,
        (unsigned long long)g_wasapi_diag.totalMixedSamples);
    buf[sizeof(buf) - 1] = '\0';
    OutputDebugStringA(buf);

    g_wasapi_diag.processCalls = 0;
    g_wasapi_diag.underrunWrites = 0;
    g_wasapi_diag.partialWrites = 0;
    g_wasapi_diag.totalRequestedSamples = 0;
    g_wasapi_diag.totalMixedSamples = 0;
    g_wasapi_diag.totalSilentSamples = 0;
}

}

CWasapi::CWasapi(void)
{
    device = nullptr;
    audioClient = nullptr;
    renderClient = nullptr;
    sampleEvent = NULL;
    initDone = false;
    bufferFrames = 0;
    bytesPerFrame = 4;
    volume = 1.0;
}

CWasapi::~CWasapi(void)
{
    DeInitSoundOutput();
}

bool CWasapi::InitSoundOutput(void)
{
    return true;
}

void CWasapi::ReleaseClient()
{
    if (audioClient)
        audioClient->Stop();

    SafeRelease(renderClient);
    SafeRelease(audioClient);
    SafeRelease(device);

    if (sampleEvent)
    {
        CloseHandle(sampleEvent);
        sampleEvent = NULL;
    }

    initDone = false;
}

void CWasapi::DeInitSoundOutput(void)
{
    ReleaseClient();
}

bool CWasapi::InitClient(bool exclusive)
{
    IMMDeviceEnumerator *enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr) || !enumerator)
        return false;

    int selected = FindDeviceIndex(GUI.AudioDevice);

    if (selected <= 0)
    {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    else
    {
        IMMDeviceCollection *collection = nullptr;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
        if (SUCCEEDED(hr) && collection)
        {
            UINT count = 0;
            collection->GetCount(&count);
            if ((UINT)(selected - 1) < count)
                hr = collection->Item(selected - 1, &device);
            else
                hr = E_FAIL;
        }
        SafeRelease(collection);
    }

    SafeRelease(enumerator);

    if (FAILED(hr) || !device)
        return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&audioClient);
    if (FAILED(hr) || !audioClient)
        return false;

    WAVEFORMATEX mixFormat{};
    mixFormat.wFormatTag = WAVE_FORMAT_PCM;
    mixFormat.nChannels = 2;
    mixFormat.nSamplesPerSec = Settings.SoundPlaybackRate;
    mixFormat.wBitsPerSample = 16;
    mixFormat.nBlockAlign = (mixFormat.nChannels * mixFormat.wBitsPerSample) / 8;
    mixFormat.nAvgBytesPerSec = mixFormat.nSamplesPerSec * mixFormat.nBlockAlign;
    mixFormat.cbSize = 0;

    bytesPerFrame = mixFormat.nBlockAlign;

    REFERENCE_TIME defaultPeriod = 0;
    REFERENCE_TIME minPeriod = 0;
    audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

    REFERENCE_TIME requestedBuffer = (REFERENCE_TIME)GUI.SoundBufferSize * 10000;
    if (requestedBuffer <= 0)
        requestedBuffer = defaultPeriod;

    if (!exclusive)
    {
        const REFERENCE_TIME minSharedBuffer = 200000; // 20ms floor for live-stream stability in shared mode
        if (requestedBuffer < minSharedBuffer)
            requestedBuffer = minSharedBuffer;
    }

    if (Settings.SoundSync && GUI.ReduceInputLag)
    {
        const REFERENCE_TIME minLowLatencyBuffer = 80000;   // 8ms
        const REFERENCE_TIME maxLowLatencyBuffer = 240000;  // 24ms
        if (requestedBuffer < minLowLatencyBuffer)
            requestedBuffer = minLowLatencyBuffer;
        if (requestedBuffer > maxLowLatencyBuffer)
            requestedBuffer = maxLowLatencyBuffer;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;

    if (exclusive)
    {
        if (requestedBuffer < minPeriod)
            requestedBuffer = minPeriod;
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     flags,
                                     requestedBuffer,
                                     requestedBuffer,
                                     &mixFormat,
                                     NULL);
    }
    else
    {
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                     flags,
                                     requestedBuffer,
                                     0,
                                     &mixFormat,
                                     NULL);
    }

    if (FAILED(hr))
        return false;

    sampleEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!sampleEvent)
        return false;

    hr = audioClient->SetEventHandle(sampleEvent);
    if (FAILED(hr))
        return false;

    hr = audioClient->GetService(__uuidof(IAudioRenderClient), (void **)&renderClient);
    if (FAILED(hr) || !renderClient)
        return false;

    hr = audioClient->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0)
        return false;

    BYTE *data = nullptr;
    hr = renderClient->GetBuffer(bufferFrames, &data);
    if (SUCCEEDED(hr) && data)
    {
        memset(data, 0, bufferFrames * bytesPerFrame);
        renderClient->ReleaseBuffer(bufferFrames, 0);
    }

    hr = audioClient->Start();
    if (FAILED(hr))
        return false;

    initDone = true;
    return true;
}

bool CWasapi::SetupSound(void)
{
    g_wasapi_diag.enabled = S9xWasapiDiagEnabled();
    g_wasapi_diag.lastLogTick = GetTickCount64();
    g_wasapi_diag.processCalls = 0;
    g_wasapi_diag.underrunWrites = 0;
    g_wasapi_diag.partialWrites = 0;
    g_wasapi_diag.totalRequestedSamples = 0;
    g_wasapi_diag.totalMixedSamples = 0;
    g_wasapi_diag.totalSilentSamples = 0;
    ReleaseClient();
    return InitClient(false);
}

void CWasapi::SetVolume(double newVolume)
{
    volume = (std::max)(0.0, (std::min)(1.0, newVolume));
}

void CWasapi::ProcessSound()
{
    if (!initDone || !audioClient || !renderClient)
        return;

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
        return;

    UINT32 availableFrames = (padding < bufferFrames) ? (bufferFrames - padding) : 0;
    if (availableFrames == 0)
        return;

    // Only submit frames we actually have samples for — never fabricate audio.
    UINT32 availableSamples = S9xGetSampleCount();
    // Ensure even sample count (stereo pairs)
    availableSamples &= ~1U;
    UINT32 framesToWrite = (std::min)(availableFrames, availableSamples / 2);

    if (framesToWrite == 0)
    {
        // Feed DRC even when we have nothing to submit so it can speed up production.
        if (Settings.DynamicRateControl)
            S9xUpdateDynamicRate((int)(availableFrames * bytesPerFrame),
                                (int)(bufferFrames * bytesPerFrame));
        return;
    }

    BYTE *data = nullptr;
    if (FAILED(renderClient->GetBuffer(framesToWrite, &data)) || !data)
        return;

    UINT32 samplesToMix = framesToWrite * 2;
    S9xMixSamples(data, samplesToMix);

    // Volume scaling
    if (volume < 0.999)
    {
        int16_t *samples = (int16_t *)data;
        for (UINT32 i = 0; i < samplesToMix; i++)
        {
            int32_t scaled = (int32_t)(samples[i] * volume);
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            samples[i] = (int16_t)scaled;
        }
    }

    renderClient->ReleaseBuffer(framesToWrite, 0);

    // Update DRC AFTER submitting audio — this adjusts the resampler ratio
    // for future production based on current WASAPI buffer occupancy.
    if (Settings.DynamicRateControl)
    {
        // Recompute free space after our write
        int freeBytes = (int)((availableFrames - framesToWrite) * bytesPerFrame);
        int totalBytes = (int)(bufferFrames * bytesPerFrame);
        S9xUpdateDynamicRate(freeBytes, totalBytes);
    }

    g_wasapi_diag.processCalls++;
    g_wasapi_diag.totalRequestedSamples += framesToWrite * 2;
    g_wasapi_diag.totalMixedSamples += samplesToMix;
    S9xWasapiDiagLog(bufferFrames, padding);
}

std::vector<std::wstring> CWasapi::GetDeviceList()
{
    std::vector<std::wstring> list;
    list.push_back(L"Default");

    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDeviceCollection *collection = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void **)&enumerator);
    if (FAILED(hr) || !enumerator)
        return list;

    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection)
    {
        SafeRelease(collection);
        SafeRelease(enumerator);
        return list;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; i++)
    {
        IMMDevice *endpoint = nullptr;
        IPropertyStore *props = nullptr;
        PROPVARIANT val;
        PropVariantInit(&val);

        if (collection->Item(i, &endpoint) == S_OK)
        {
            if (endpoint->OpenPropertyStore(STGM_READ, &props) == S_OK)
            {
                if (props->GetValue(PKEY_Device_FriendlyName, &val) == S_OK && val.vt == VT_LPWSTR)
                    list.push_back(val.pwszVal);
            }
        }

        PropVariantClear(&val);
        SafeRelease(props);
        SafeRelease(endpoint);
    }

    SafeRelease(collection);
    SafeRelease(enumerator);

    return list;
}

int CWasapi::FindDeviceIndex(TCHAR *audio_device)
{
    std::vector<std::wstring> device_list = GetDeviceList();

    int index = 0;
    for (int i = 0; i < (int)device_list.size(); i++)
    {
        if (_tcsstr(device_list[i].c_str(), audio_device) != NULL)
        {
            index = i;
            break;
        }
    }

    return index;
}
