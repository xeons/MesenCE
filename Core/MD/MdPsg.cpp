#include "pch.h"
#include "MD/MdPsg.h"
#include "MD/MdConsole.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/Audio/SoundMixer.h"
#include "Utilities/Serializer.h"

MdPsg::MdPsg(Emulator* emu, MdConsole* console)
{
	_console = console;
	_soundMixer = emu->GetSoundMixer();
	_settings = emu->GetSettings();

	_state.Noise.Lfsr = 0x8000;
	_state.Noise.Volume = 0x0F;
	_state.Tone[0].Volume = 0x0F;
	_state.Tone[1].Volume = 0x0F;
	_state.Tone[2].Volume = 0x0F;

	_soundBuffer = new int16_t[MdPsg::MaxSamples * 2];
	memset(_soundBuffer, 0, MdPsg::MaxSamples * 2 * sizeof(int16_t));

	_buffer = blip_new(MdPsg::MaxSamples);
	SetRegion(ConsoleRegion::Ntsc);
}

MdPsg::~MdPsg()
{
	blip_delete(_buffer);
	delete[] _soundBuffer;
}

void MdPsg::SetRegion(ConsoleRegion region)
{
	blip_clear(_buffer);
	blip_set_rates(_buffer, (double)_console->GetMasterClockRate() / MdConstants::Z80Divider, MdPsg::SampleRate);
	_clockCounter = 0;
}

void MdPsg::RunNoise(MdNoiseChannelState& noise)
{
	if(noise.Timer == 0 || --noise.Timer == 0) {
		noise.Output ^= 1;
		switch(noise.Control & 0x03) {
			case 0: noise.Timer = 0x10; break;
			case 1: noise.Timer = 0x20; break;
			case 2: noise.Timer = 0x40; break;
			case 3: noise.Timer = _state.Tone[2].ReloadValue; break;
		}

		if(noise.Output) {
			//16-bit LFSR, tapped on bits 0 and 3 for white noise
			bool useBit3 = noise.Control & 0x04;
			uint16_t newBit = (noise.Lfsr & 0x01) ^ (useBit3 & ((noise.Lfsr >> 3) & 0x01));
			noise.Lfsr = (newBit << 15) | (noise.Lfsr >> 1);
		}
	}
}

void MdPsg::Run()
{
	uint64_t runTo = _console->GetMasterClock() / MdConstants::Z80Divider;
	uint32_t volume = _settings->GetMdConfig().PsgVolume;

	while(_clock + 16 <= runTo) {
		int32_t output = 0;
		for(int i = 0; i < 3; i++) {
			if(_state.Tone[i].Timer == 0 || --_state.Tone[i].Timer == 0) {
				_state.Tone[i].Output ^= 1;
				_state.Tone[i].Timer = _state.Tone[i].ReloadValue;
			}
			output += _state.Tone[i].Output * _volumeLut[_state.Tone[i].Volume];
		}

		RunNoise(_state.Noise);
		output += (_state.Noise.Lfsr & 0x01) * _volumeLut[_state.Noise.Volume];
		output = output * (int32_t)volume / 100;

		_clockCounter += 16;
		_clock += 16;

		if(_prevOutput != output) {
			blip_add_delta(_buffer, (uint32_t)_clockCounter, output - _prevOutput);
			_prevOutput = (int16_t)output;
		}
	}

	if(_clockCounter >= 20000) {
		PlayQueuedAudio();
	}
}

void MdPsg::PlayQueuedAudio()
{
	blip_end_frame(_buffer, (uint32_t)_clockCounter);
	uint32_t sampleCount = (uint32_t)blip_read_samples(_buffer, _soundBuffer, MdPsg::MaxSamples, 1);
	for(uint32_t i = 0; i < sampleCount; i++) {
		//Mono output
		_soundBuffer[i * 2 + 1] = _soundBuffer[i * 2];
	}

	_soundMixer->PlayAudioBuffer(_soundBuffer, sampleCount, MdPsg::SampleRate);
	_clockCounter = 0;
}

void MdPsg::Write(uint8_t value)
{
	Run();

	if(value & 0x80) {
		_state.SelectedReg = (value >> 4) & 0x07;
	}

	uint8_t channel = (_state.SelectedReg >> 1) & 0x03;
	bool volReg = _state.SelectedReg & 0x01;

	switch(channel) {
		case 0:
		case 1:
		case 2:
			if(volReg) {
				_state.Tone[channel].Volume = value & 0x0F;
			} else {
				if(value & 0x80) {
					_state.Tone[channel].ReloadValue = (_state.Tone[channel].ReloadValue & 0x3F0) | (value & 0x0F);
				} else {
					_state.Tone[channel].ReloadValue = (_state.Tone[channel].ReloadValue & 0x0F) | ((value & 0x3F) << 4);
				}
			}
			break;

		case 3:
			if(volReg) {
				_state.Noise.Volume = value & 0x0F;
			} else {
				_state.Noise.Control = value & 0x07;
				_state.Noise.Lfsr = 0x8000;
			}
			break;
	}
}

void MdPsg::Serialize(Serializer& s)
{
	if(s.IsSaving()) {
		Run();
	} else {
		_clockCounter = 0;
		blip_clear(_buffer);
	}

	SV(_state.SelectedReg);

	SV(_state.Noise.Timer);
	SV(_state.Noise.Lfsr);
	SV(_state.Noise.Control);
	SV(_state.Noise.Output);
	SV(_state.Noise.Volume);

	for(int i = 0; i < 3; i++) {
		SVI(_state.Tone[i].ReloadValue);
		SVI(_state.Tone[i].Timer);
		SVI(_state.Tone[i].Output);
		SVI(_state.Tone[i].Volume);
	}

	if(s.GetFormat() != SerializeFormat::Map) {
		//Hide these entries from the Lua API
		SV(_clock);
		SV(_clockCounter);
		SV(_prevOutput);
	}
}
