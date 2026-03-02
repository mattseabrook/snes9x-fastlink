import sys

old_code = """void CWasapi::ProcessSound()
{
    if (!initDone || !audioClient || !renderClient)
        return;

    const DWORD syncWaitMs = GUI.ReduceInputLag ? 0u : 1u;

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
        if (Settings.SoundSync && !Settings.TurboMode && !Settings.Mute)
            WaitForSingleObject(sampleEvent, syncWaitMs);

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

    if (Settings.SoundSync && !Settings.TurboMode && !Settings.Mute && availableSamples > writableSamples)
    {
        WaitForSingleObject(sampleEvent, syncWaitMs);
    }

    LeaveCriticalSection(&GUI.SoundCritSect);
}"""

new_code = """void CWasapi::ProcessSound()
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
    UINT32 availableSamples = S9xGetSampleCount();

    if (Settings.DynamicRateControl && !Settings.SoundSync)
    {
        if (availableSamples > availableFrames * 2) {
            S9xClearSamples();
            LeaveCriticalSection(&GUI.SoundCritSect);
            return;
        }
    }

    if (Settings.SoundSync && !Settings.TurboMode && !Settings.Mute)
    {
        const DWORD syncWaitMs = GUI.ReduceInputLag ? 2u : (DWORD)max(4u, min(50u, (unsigned int)(Settings.FrameTime / 1000 + 4)));
        const int maxSyncRetries = GUI.ReduceInputLag ? 2 : 4;
        int syncRetries = 0;

        while (availableFrames * 2 < availableSamples)
        {
            ResetEvent(sampleEvent);
            LeaveCriticalSection(&GUI.SoundCritSect);

            if (!GUI.AllowSoundSync || WaitForSingleObject(sampleEvent, syncWaitMs) != WAIT_OBJECT_0)
            {
                EnterCriticalSection(&GUI.SoundCritSect);
                S9xClearSamples();
                LeaveCriticalSection(&GUI.SoundCritSect);
                return;
            }

            EnterCriticalSection(&GUI.SoundCritSect);
            
            if (FAILED(audioClient->GetCurrentPadding(&padding))) break;
            availableFrames = (padding < bufferFrames) ? (bufferFrames - padding) : 0;

            if (syncRetries++ > maxSyncRetries)
            {
                S9xClearSamples();
                LeaveCriticalSection(&GUI.SoundCritSect);
                return;
            }
        }
    }

    if (availableFrames > 0)
    {
        const int freeBytes = (int)(availableFrames * bytesPerFrame);
        const int totalBytes = (int)(bufferFrames * bytesPerFrame);

        if (Settings.DynamicRateControl)
            S9xUpdateDynamicRate(freeBytes, totalBytes);

        availableSamples = S9xGetSampleCount();
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
    }

    LeaveCriticalSection(&GUI.SoundCritSect);
}"""

import pathlib
content = pathlib.Path('win32/CWasapi.cpp').read_text()
if old_code in content:
    pathlib.Path('win32/CWasapi.cpp').write_text(content.replace(old_code, new_code))
    print("Patched WASAPI")
else:
    print("Did not find old_code")
