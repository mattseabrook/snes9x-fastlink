/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include "IS9xSoundOutput.h"
#include "../snes9x.h"
#include "../apu/apu.h"
#include "wsnes9x.h"
#include "CWasapi.h"
#include "win32_sound.h"
#include "win32_display.h"
#include <atomic>
#include <thread>

#define CLAMP(x, low, high) (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

// available sound output methods
CWasapi S9xWasapi;

// Interface used to access the sound output
IS9xSoundOutput *S9xSoundOutput = &S9xWasapi;

static double last_volume = 1.0;
static std::thread sound_worker_thread;
static std::atomic<bool> sound_worker_running = false;
static HANDLE sound_worker_event = NULL;

static void StopSoundWorker()
{
	if (sound_worker_running.exchange(false))
	{
		if (sound_worker_event)
			SetEvent(sound_worker_event);
		if (sound_worker_thread.joinable())
			sound_worker_thread.join();
	}

	if (sound_worker_event)
	{
		CloseHandle(sound_worker_event);
		sound_worker_event = NULL;
	}
}

static bool StartSoundWorker()
{
	if (sound_worker_running)
		return true;

	sound_worker_event = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (!sound_worker_event)
		return false;

	sound_worker_running = true;
	sound_worker_thread = std::thread([]()
	{
		while (sound_worker_running)
		{
			WaitForSingleObject(sound_worker_event, 2);

			if (!sound_worker_running)
				break;

			S9xSoundOutput->ProcessSound();

			if (S9xGetSampleCount() > 0)
				S9xSoundOutput->ProcessSound();
		}
	});

	return true;
}

/*  ReInitSound
reinitializes the sound core with current settings
IN:
mode		-	0 disables sound output, 1 enables
-----
returns true if successful, false otherwise
*/
bool ReInitSound()
{
	if (GUI.AVIOut)
		return false;
	if (GUI.AutomaticInputRate)
	{
		int rate = WinGetAutomaticInputRate();
		if (rate)
			Settings.SoundInputRate = rate;
		else
		{
			GUI.AutomaticInputRate = false;
			Settings.SoundInputRate = 31950;
		}
	}

	Settings.SoundInputRate = CLAMP(Settings.SoundInputRate,31700, 32300);
	Settings.SoundPlaybackRate = CLAMP(Settings.SoundPlaybackRate,8000, 48000);
	Settings.SoundSync = false;
	S9xSetSoundMute(GUI.Mute);
	StopSoundWorker();
	if(S9xSoundOutput)
		S9xSoundOutput->DeInitSoundOutput();

    last_volume = 1.0;
    return S9xInitSound(25);
}

void CloseSoundDevice() {
	StopSoundWorker();
	S9xSoundOutput->DeInitSoundOutput();
	S9xSetSamplesAvailableCallback(NULL,NULL);
}

/*  S9xOpenSoundDevice
called by S9xInitSound - initializes the currently selected sound output and
applies the current sound settings
-----
returns true if successful, false otherwise
*/
bool8 S9xOpenSoundDevice ()
{
	S9xSetSamplesAvailableCallback (NULL, NULL);

	// WASAPI-only low-latency output path.
	GUI.SoundDriver = WIN_WASAPI_SOUND_DRIVER;
	S9xSoundOutput = &S9xWasapi;
	if (!S9xSoundOutput->InitSoundOutput() || !S9xSoundOutput->SetupSound())
		return false;

	if (!StartSoundWorker())
	{
		S9xSoundOutput->DeInitSoundOutput();
		return false;
	}
	
	S9xSetSamplesAvailableCallback (S9xSoundCallback, NULL);
	return true;
}

/*  S9xSoundCallback
called by the sound core to process generated samples
*/
void S9xSoundCallback(void *data)
{
	(void)data;

	// only try to change volume if we actually need to switch it
	double current_volume = ((Settings.TurboMode || Settings.Rewinding) ? GUI.VolumeTurbo : GUI.VolumeRegular) / 100.;
	if (last_volume != current_volume) {
		S9xSoundOutput->SetVolume(current_volume);
		last_volume = current_volume;
	}

	if (sound_worker_event)
		SetEvent(sound_worker_event);
}

void S9xFinalizeSamples(void)
{
    if (sound_worker_event)
        SetEvent(sound_worker_event);
}

/*  GetAvailableSoundDevices
returns a list of output devices available for the current output driver
*/
std::vector<std::wstring> GetAvailableSoundDevices()
{
    return S9xSoundOutput->GetDeviceList();
}

/*  FindAudioDeviceIndex
find an audio device that matches the currently configured audio device string
*/
int FindAudioDeviceIndex(TCHAR *audio_device)
{
    return S9xSoundOutput->FindDeviceIndex(audio_device);
}
