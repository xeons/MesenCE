#include "pch.h"
#include "MD/MdConsole.h"
#include "MD/MdCpu.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdPsg.h"
#include "MD/MdFmAudio.h"
#include "MD/MdCart.h"
#include "MD/MdControlManager.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdDefaultVideoFilter.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/CheatManager.h"
#include "Utilities/Serializer.h"
#include "Utilities/StringUtilities.h"

MdConsole::MdConsole(Emulator* emu)
{
	_emu = emu;
}

MdConsole::~MdConsole()
{
}

LoadRomResult MdConsole::LoadRom(VirtualFile& romFile)
{
	vector<uint8_t> romData;
	romFile.ReadFile(romData);

	if(romData.size() < 0x200) {
		return LoadRomResult::Failure;
	}

	bool isSmd = romFile.GetFileExtension() == ".smd" || ((romData.size() % 0x4000) == 0x200 && romData[8] == 0xAA && romData[9] == 0xBB);
	if(isSmd) {
		MdCart::DeinterleaveSmd(romData);
		if(romData.size() < 0x200) {
			return LoadRomResult::Failure;
		}
	}

	_filename = romFile.GetFileName();

	_cart.reset(new MdCart(_emu, this, romData));
	_memoryManager.reset(new MdMemoryManager());
	_cpu.reset(new MdCpu());
	_z80.reset(new MdZ80Cpu());
	_vdp.reset(new MdVdp());
	_psg.reset(new MdPsg(_emu, this));
	_fmAudio.reset(new MdFmAudio(_emu, this));
	_controlManager.reset(new MdControlManager(_emu, this));

	_memoryManager->Init(_emu, this, _cpu.get(), _z80.get(), _vdp.get(), _cart.get(), _psg.get(), _fmAudio.get(), _controlManager.get());
	_vdp->Init(_emu, this, _cpu.get(), _z80.get(), _memoryManager.get());
	_z80->Init(_emu, this, _memoryManager.get());

	UpdateRegion(true);

	_cpu->Init(_emu, this, _memoryManager.get());
	_cpu->Reset();

	return LoadRomResult::Success;
}

void MdConsole::Reset()
{
	//The reset button resets the 68000, the Z80 and the YM2612 - VDP and RAM contents are left untouched
	_z80->SetBusRequest(false);
	_z80->SetReset(true);
	_fmAudio->Reset();
	_cpu->Reset();
}

void MdConsole::RunFrame()
{
	UpdateRegion(false);

	uint32_t frame = _vdp->GetFrameCount();
	while(frame == _vdp->GetFrameCount()) {
		_cpu->Exec();
	}

	_psg->Run();
	_psg->PlayQueuedAudio();
}

void MdConsole::ProcessEndOfFrame()
{
	_controlManager->UpdateControlDevices();
	_controlManager->UpdateInputState();
}

ConsoleRegion MdConsole::GetHeaderRegion()
{
	ConsoleRegion region = _cart->GetHeaderRegion();

	//Use the filename to pick between regions for multi-region games
	string filename = StringUtilities::ToLower(_filename);
	if(filename.find("(europe)") != string::npos || filename.find("(e)") != string::npos) {
		return ConsoleRegion::Pal;
	} else if(filename.find("(japan)") != string::npos || filename.find("(j)") != string::npos) {
		return ConsoleRegion::NtscJapan;
	}

	return region;
}

void MdConsole::UpdateRegion(bool forceUpdate)
{
	ConsoleRegion region = _emu->GetSettings()->GetMdConfig().Region;
	if(region == ConsoleRegion::Auto || region == ConsoleRegion::Dendy) {
		region = GetHeaderRegion();
	}

	if(_region != region || forceUpdate) {
		_region = region;
		_vdp->SetRegion(_region);
		_psg->SetRegion(_region);
	}
}

void MdConsole::SaveBattery()
{
	_cart->SaveBattery();
}

BaseControlManager* MdConsole::GetControlManager()
{
	return _controlManager.get();
}

ConsoleRegion MdConsole::GetRegion()
{
	return _region;
}

ConsoleType MdConsole::GetConsoleType()
{
	return ConsoleType::MegaDrive;
}

vector<CpuType> MdConsole::GetCpuTypes()
{
	return { CpuType::Md, CpuType::MdZ80 };
}

uint64_t MdConsole::GetMasterClock()
{
	return _memoryManager->GetMasterClock();
}

uint32_t MdConsole::GetMasterClockRate()
{
	return _region == ConsoleRegion::Pal ? MdConstants::PalMasterClockRate : MdConstants::NtscMasterClockRate;
}

double MdConsole::GetFps()
{
	if(_region == ConsoleRegion::Pal) {
		return (double)MdConstants::PalMasterClockRate / (MdConstants::ClocksPerScanline * MdConstants::PalScanlineCount);
	} else {
		return (double)MdConstants::NtscMasterClockRate / (MdConstants::ClocksPerScanline * MdConstants::NtscScanlineCount);
	}
}

BaseVideoFilter* MdConsole::GetVideoFilter(bool getDefaultFilter)
{
	//TODOMD NTSC filter
	return new MdDefaultVideoFilter(_emu, this);
}

PpuFrameInfo MdConsole::GetPpuFrame()
{
	PpuFrameInfo frame = {};
	frame.FirstScanline = 0;
	frame.FrameCount = _vdp->GetFrameCount();
	frame.Width = _vdp->GetScreenWidth();
	frame.Height = _vdp->GetScreenHeight();
	frame.ScanlineCount = _vdp->GetScanlineCount();
	frame.CycleCount = MdConstants::ClocksPerScanline;
	frame.FrameBufferSize = frame.Width * frame.Height * sizeof(uint16_t);
	//The VDP swaps buffers as soon as a frame is completed
	frame.FrameBuffer = (uint8_t*)_vdp->GetScreenBuffer(true);
	return frame;
}

uint32_t MdConsole::GetFrameCount()
{
	return _vdp->GetFrameCount();
}

RomFormat MdConsole::GetRomFormat()
{
	return RomFormat::Md;
}

AudioTrackInfo MdConsole::GetAudioTrackInfo()
{
	return AudioTrackInfo();
}

void MdConsole::ProcessAudioPlayerAction(AudioPlayerActionParams p)
{
}

void MdConsole::RefreshRamCheats()
{
	for(InternalCheatCode& code : _emu->GetCheatManager()->GetRamRefreshCheats(CpuType::Md)) {
		_memoryManager->DebugWrite(code.Address, code.Value);
	}
}

AddressInfo MdConsole::GetAbsoluteAddress(AddressInfo& relAddress)
{
	if(relAddress.Type == MemoryType::MdZ80Memory) {
		return _memoryManager->GetZ80AbsoluteAddress(relAddress.Address);
	}
	return _memoryManager->GetAbsoluteAddress(relAddress.Address);
}

AddressInfo MdConsole::GetRelativeAddress(AddressInfo& absAddress, CpuType cpuType)
{
	if(cpuType == CpuType::MdZ80) {
		return { _memoryManager->GetZ80RelativeAddress(absAddress), MemoryType::MdZ80Memory };
	}
	return { _memoryManager->GetRelativeAddress(absAddress), MemoryType::MdMemory };
}

MdState MdConsole::GetState()
{
	MdState state;
	state.Cpu = _cpu->GetState();
	state.Z80 = _z80->GetState();
	state.Vdp = _vdp->GetState();
	state.Psg = _psg->GetState();
	state.Fm = _fmAudio->GetState();
	state.MemoryManager = _memoryManager->GetState();
	state.ControlManager = _controlManager->GetState();
	return state;
}

void MdConsole::GetConsoleState(BaseState& state, ConsoleType consoleType)
{
	(MdState&)state = GetState();
}

void MdConsole::InitializeRam(void* data, uint32_t length)
{
	EmuSettings* settings = _emu->GetSettings();
	settings->InitializeRam(settings->GetMdConfig().RamPowerOnState, data, length);
}

void MdConsole::Serialize(Serializer& s)
{
	SV(_cpu);
	SV(_z80);
	SV(_vdp);
	SV(_controlManager);
	SV(_cart); //Process cart before memory manager to ensure mappings are updated properly
	SV(_memoryManager);
	SV(_psg);
	SV(_fmAudio);
}
