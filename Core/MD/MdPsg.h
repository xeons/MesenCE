#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/SettingTypes.h"
#include "Utilities/ISerializable.h"
#include "Utilities/Audio/blip_buf.h"

class Emulator;
class MdConsole;
class SoundMixer;
class EmuSettings;

//SN76489-compatible PSG integrated into the VDP
class MdPsg final : public ISerializable
{
private:
	static constexpr int SampleRate = 96000;
	static constexpr int MaxSamples = 4000;

	//Same curve as the SMS' PSG, scaled down to keep a realistic balance with the YM2612's output
	static constexpr int16_t _volumeLut[16] = { 5400, 4290, 3407, 2706, 2150, 1708, 1357, 1082, 856, 680, 540, 429, 341, 270, 215, 0 };

	int16_t* _soundBuffer = nullptr;
	blip_t* _buffer = nullptr;

	SoundMixer* _soundMixer = nullptr;
	EmuSettings* _settings = nullptr;
	MdConsole* _console = nullptr;

	MdPsgState _state = {};
	uint64_t _clock = 0; //In PSG input clocks (MCLK/15)
	uint64_t _clockCounter = 0;
	int16_t _prevOutput = 0;

	void RunNoise(MdNoiseChannelState& noise);

public:
	MdPsg(Emulator* emu, MdConsole* console);
	~MdPsg();

	MdPsgState& GetState() { return _state; }

	void SetRegion(ConsoleRegion region);

	void Run();
	void PlayQueuedAudio();

	void Write(uint8_t value);

	void Serialize(Serializer& s) override;
};
