#include "pch.h"
#include "MD/MdFmAudio.h"
#include "MD/MdConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

MdFmAudio::MdFmAudio(Emulator* emu, MdConsole* console) : _opn(*this)
{
	_emu = emu;
	_console = console;
	_opn.reset();
	_emu->GetSoundMixer()->RegisterAudioProvider(this);
}

MdFmAudio::~MdFmAudio()
{
	_emu->GetSoundMixer()->UnregisterAudioProvider(this);
}

void MdFmAudio::Reset()
{
	Run();
	_opn.reset();
	_state.Timers[0] = 0;
	_state.Timers[1] = 0;
	_busyEndClock = 0;
	memset(_state.Registers, 0, sizeof(_state.Registers));
}

void MdFmAudio::UpdateTimers(uint32_t clocks)
{
	for(int i = 0; i < 2; i++) {
		if(_state.Timers[i]) {
			if(_state.Timers[i] <= clocks) {
				_state.Timers[i] = 0;
				//Reloads the timer via ymfm_set_timer, if it is still enabled
				m_engine->engine_timer_expired(i);
			} else {
				_state.Timers[i] -= clocks;
			}
		}
	}
}

void MdFmAudio::Run()
{
	uint64_t masterClock = _console->GetMasterClock();
	bool useYm2612 = _emu->GetSettings()->GetMdConfig().FmChip == MdFmChip::Ym2612;

	while(_prevMasterClock + MdConstants::FmSampleDivider <= masterClock) {
		//One sample is generated every 144 FM clocks (6 prescaler * 24 operator slots)
		UpdateTimers(MdConstants::FmSampleDivider / MdConstants::CpuDivider);

		ymfm::ym2612::output_data output;
		if(useYm2612) {
			_opn.ym2612::generate(&output, 1);
		} else {
			_opn.generate(&output, 1);
		}

		for(int i = 0; i < 2; i++) {
			//~5Hz high-pass filter at the FM sample rate
			double input = output.data[i];
			double filtered = input - _prevInput[i] + 0.9994 * _prevOutput[i];
			_prevInput[i] = input;
			_prevOutput[i] = filtered;
			_samplesToPlay.push_back((int16_t)std::clamp<int32_t>((int32_t)filtered, INT16_MIN, INT16_MAX));
		}
		_prevMasterClock += MdConstants::FmSampleDivider;
	}
}

uint8_t MdFmAudio::Read(uint8_t addr)
{
	Run();
	return _opn.read(addr & 0x03);
}

void MdFmAudio::Write(uint8_t addr, uint8_t value)
{
	Run();

	addr &= 0x03;
	if(addr & 0x01) {
		_state.Registers[addr >> 1][_state.SelectedAddress[addr >> 1]] = value;
	} else {
		_state.SelectedAddress[addr >> 1] = value;
	}

	_opn.write(addr, value);
}

void MdFmAudio::ymfm_set_timer(uint32_t timerId, int32_t durationInClocks)
{
	_state.Timers[timerId] = durationInClocks < 0 ? 0 : (uint32_t)durationInClocks;
}

void MdFmAudio::ymfm_set_busy_end(uint32_t clocks)
{
	_busyEndClock = _console->GetMasterClock() + (uint64_t)clocks * MdConstants::CpuDivider;
}

bool MdFmAudio::ymfm_is_busy()
{
	return _console->GetMasterClock() < _busyEndClock;
}

void MdFmAudio::MixAudio(int16_t* out, uint32_t sampleCount, uint32_t sampleRate)
{
	Run();

	_resampler.SetSampleRates((double)_console->GetMasterClockRate() / MdConstants::FmSampleDivider, sampleRate);
	_resampler.SetVolume(_emu->GetSettings()->GetMdConfig().FmVolume / 100.0);
	_resampler.Resample<true>(_samplesToPlay.data(), (uint32_t)_samplesToPlay.size() / 2, out, sampleCount, true);
	_samplesToPlay.clear();
}

void MdFmAudio::Serialize(Serializer& s)
{
	vector<uint8_t> opnData;
	ymfm::ymfm_saved_state state = ymfm::ymfm_saved_state(opnData, s.IsSaving());

	SVArray(_state.SelectedAddress, 2);
	SVArray(_state.Registers[0], 0x100);
	SVArray(_state.Registers[1], 0x100);
	SVArray(_state.Timers, 2);
	SV(_prevMasterClock);
	SV(_busyEndClock);

	if(s.IsSaving()) {
		_opn.save_restore(state);
		SVVector(opnData);
	} else {
		SVVector(opnData);
		_opn.save_restore(state);
		_samplesToPlay.clear();
	}
}
