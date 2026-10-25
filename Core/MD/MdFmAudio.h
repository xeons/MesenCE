#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/Interfaces/IAudioProvider.h"
#include "Utilities/Audio/HermiteResampler.h"
#include "Utilities/Audio/ymfm/ymfm_opn.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;

class MdFmAudio final : public ymfm::ymfm_interface, public ISerializable, public IAudioProvider
{
private:
	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;

	//The YM3438 only differs by its generate() function (no DAC ladder distortion), ym2612::generate is used for the YM2612
	ymfm::ym3438 _opn;
	MdFmState _state = {};

	HermiteResampler _resampler;
	vector<int16_t> _samplesToPlay;
	uint64_t _prevMasterClock = 0;
	uint64_t _busyEndClock = 0;

	//DC blocking filter, simulates the output's coupling capacitors (the YM2612's DAC output has a large DC offset)
	double _prevInput[2] = {};
	double _prevOutput[2] = {};

	void UpdateTimers(uint32_t clocks);

public:
	MdFmAudio(Emulator* emu, MdConsole* console);
	~MdFmAudio();

	void Reset();
	void Run();

	uint8_t Read(uint8_t addr);
	void Write(uint8_t addr, uint8_t value);

	MdFmState& GetState() { return _state; }

	void ymfm_set_timer(uint32_t timerId, int32_t durationInClocks) override;
	void ymfm_set_busy_end(uint32_t clocks) override;
	bool ymfm_is_busy() override;

	void MixAudio(int16_t* out, uint32_t sampleCount, uint32_t sampleRate) override;

	void Serialize(Serializer& s) override;
};
