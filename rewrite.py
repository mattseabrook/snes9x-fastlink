import re
with open("win32/CWasapi.cpp", "r", encoding="utf-8") as f:
    code = f.read()

start_marker = "void CWasapi::ProcessSound()"
end_marker = "std::vector<std::wstring> CWasapi::GetDeviceList()"

start_idx = code.find(start_marker)
end_idx = code.find(end_marker)

if start_idx != -1 and end_idx != -1:
    new_code = code[:start_idx] + r"""void CWasapi::ProcessSound()
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
        const DWORD syncWaitMs = GUI.ReduceInputLag ? 2u : (DWORD)((std::max)(4u, (std::min)(50u, (unsigned int)(Settings.FrameTime / 1000 + 4))));
        const int maxSyncRetries = GUI.ReduceInputLag ? 1 : 3;
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
}

""" + code[end_idx:]

    with open("win32/CWasapi.cpp", "w", encoding="utf-8") as f:
        f.write(new_code)
    print("Rewritten successfully")
else:
    print("Markers not found")
