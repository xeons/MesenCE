#pragma once
#include "pch.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/SettingTypes.h"
#include "MD/MdTypes.h"

class Emulator;
class VirtualFile;
class BaseControlManager;

class MdCpu;
class MdZ80;
class MdVdp;
class MdPsg;
class MdFmAudio;
class MdCart;
class MdControlManager;
class MdMemoryManager;

class MdConsole final : public IConsole
{
private:
	Emulator* _emu = nullptr;
	unique_ptr<MdCpu> _cpu;
	unique_ptr<MdZ80> _z80;
	unique_ptr<MdVdp> _vdp;
	unique_ptr<MdPsg> _psg;
	unique_ptr<MdFmAudio> _fmAudio;
	unique_ptr<MdCart> _cart;
	unique_ptr<MdControlManager> _controlManager;
	unique_ptr<MdMemoryManager> _memoryManager;

	ConsoleRegion _region = ConsoleRegion::Ntsc;
	string _filename;

	void UpdateRegion(bool forceUpdate);
	ConsoleRegion GetHeaderRegion();

public:
	static vector<string> GetSupportedExtensions() { return { ".md", ".gen", ".smd", ".bin" }; }
	static vector<string> GetSupportedSignatures() { return {}; }

	MdConsole(Emulator* emu);
	virtual ~MdConsole();

	Emulator* GetEmulator() { return _emu; }
	MdCpu* GetCpu() { return _cpu.get(); }
	MdZ80* GetZ80() { return _z80.get(); }
	MdVdp* GetVdp() { return _vdp.get(); }
	MdPsg* GetPsg() { return _psg.get(); }
	MdFmAudio* GetFmAudio() { return _fmAudio.get(); }
	MdCart* GetCart() { return _cart.get(); }
	MdMemoryManager* GetMemoryManager() { return _memoryManager.get(); }

	bool IsPal() { return _region == ConsoleRegion::Pal; }
	bool IsOverseas() { return _region != ConsoleRegion::NtscJapan; }

	LoadRomResult LoadRom(VirtualFile& romFile) override;

	void Reset() override;
	void RunFrame() override;
	void ProcessEndOfFrame();

	void SaveBattery() override;

	BaseControlManager* GetControlManager() override;
	ConsoleRegion GetRegion() override;
	ConsoleType GetConsoleType() override;
	vector<CpuType> GetCpuTypes() override;
	RomFormat GetRomFormat() override;
	double GetFps() override;
	PpuFrameInfo GetPpuFrame() override;
	uint32_t GetFrameCount() override;
	BaseVideoFilter* GetVideoFilter(bool getDefaultFilter) override;

	uint64_t GetMasterClock() override;
	uint32_t GetMasterClockRate() override;

	AudioTrackInfo GetAudioTrackInfo() override;
	void ProcessAudioPlayerAction(AudioPlayerActionParams p) override;

	void RefreshRamCheats();

	AddressInfo GetAbsoluteAddress(AddressInfo& relAddress) override;
	AddressInfo GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType) override;

	MdState GetState();
	void GetConsoleState(BaseState& state, ConsoleType consoleType) override;

	void InitializeRam(void* data, uint32_t length);

	void Serialize(Serializer& s) override;
};
