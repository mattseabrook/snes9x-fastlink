/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "CWasapi.h"
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>

#include "../snes9x.h"
#include "../apu/apu.h"
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
    ReleaseClient();

    if (InitClient(true))
        return true;

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

    EnterCriticalSection(&GUI.SoundCritSect);

    UINT32 padding = 0;
    if (FAILED(audioClient->GetCurrentPadding(&padding)))
    {
        LeaveCriticalSection(&GUI.SoundCritSect);
        return;
    }

    UINT32 availableFrames = (padding < bufferFrames) ? (bufferFrames - padding) : 0;

    if (availableFrames == 0)
    {
        LeaveCriticalSection(&GUI.SoundCritSect);
        return;
    }

    const int freeBytes = (int)(availableFrames * bytesPerFrame);
    const int totalBytes = (int)(bufferFrames * bytesPerFrame);

    if (Settings.DynamicRateControl)
        S9xUpdateDynamicRate(freeBytes, totalBytes);

    UINT32 availableSamples = S9xGetSampleCount();
    BYTE *data = nullptr;
    if (FAILED(renderClient->GetBuffer(availableFrames, &data)) || !data)
    {
        LeaveCriticalSection(&GUI.SoundCritSect);
        return;
    }

    const UINT32 writableSamples = availableFrames * 2;
    UINT32 samplesToMix = (std::min)(availableSamples, writableSamples);

    if (samplesToMix > 0)
        S9xMixSamples(data, samplesToMix);

    if (samplesToMix < writableSamples)
    {
        BYTE *silent = data + samplesToMix * sizeof(int16_t);
        memset(silent, 0, (writableSamples - samplesToMix) * sizeof(int16_t));
    }

    if (volume < 0.999)
    {
        int16_t *samples = (int16_t *)data;
        for (UINT32 i = 0; i < writableSamples; i++)
        {
            int32_t scaled = (int32_t)(samples[i] * volume);
            if (scaled > 32767) scaled = 32767;
            if (scaled < -32768) scaled = -32768;
            samples[i] = (int16_t)scaled;
        }
    }

    renderClient->ReleaseBuffer(availableFrames, 0);

    LeaveCriticalSection(&GUI.SoundCritSect);
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
