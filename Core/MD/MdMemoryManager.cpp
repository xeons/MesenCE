#include "pch.h"
#include "MD/MdMemoryManager.h"
#include "MD/MdConsole.h"
#include "MD/MdCpu.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdCart.h"
#include "MD/MdPsg.h"
#include "MD/MdFmAudio.h"
#include "MD/MdControlManager.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

MdMemoryManager::MdMemoryManager()
{
}

MdMemoryManager::~MdMemoryManager()
{
	delete[] _workRam;
	delete[] _z80Ram;
}

void MdMemoryManager::Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80* z80, MdVdp* vdp, MdCart* cart, MdPsg* psg, MdFmAudio* fmAudio, MdControlManager* controlManager)
{
	_emu = emu;
	_console = console;
	_cpu = cpu;
	_z80 = z80;
	_vdp = vdp;
	_cart = cart;
	_psg = psg;
	_fmAudio = fmAudio;
	_controlManager = controlManager;

	_workRam = new uint8_t[MdConstants::WorkRamSize];
	_console->InitializeRam(_workRam, MdConstants::WorkRamSize);
	_emu->RegisterMemory(MemoryType::MdWorkRam, _workRam, MdConstants::WorkRamSize);

	_z80Ram = new uint8_t[MdConstants::Z80RamSize];
	_console->InitializeRam(_z80Ram, MdConstants::Z80RamSize);
	_emu->RegisterMemory(MemoryType::MdZ80Ram, _z80Ram, MdConstants::Z80RamSize);

	RefreshMappings();
}

void MdMemoryManager::RefreshMappings()
{
	for(int i = 0; i < 0x100; i++) {
		_readPages[i] = nullptr;
		_writePages[i] = nullptr;
		_pageTypes[i] = MdPageType::OpenBus;
	}

	//$000000-$3FFFFF: Cartridge
	for(int i = 0; i < 0x40; i++) {
		_readPages[i] = _cart->GetRomPage(i);
		_pageTypes[i] = _cart->IsSpecialPage(i) ? MdPageType::Cart : MdPageType::Direct;
	}

	//$A00000-$A0FFFF: Z80 address space
	_pageTypes[0xA0] = MdPageType::Z80;

	//$A10000-$A1FFFF: I/O and control registers
	_pageTypes[0xA1] = MdPageType::Io;

	//$C00000-$DFFFFF: VDP
	for(int i = 0xC0; i < 0xE0; i++) {
		_pageTypes[i] = MdPageType::Vdp;
	}

	//$E00000-$FFFFFF: Work RAM (64KB, mirrored)
	for(int i = 0xE0; i < 0x100; i++) {
		_readPages[i] = _workRam;
		_writePages[i] = _workRam;
		_pageTypes[i] = MdPageType::Direct;
	}
}

void MdMemoryManager::ProcessCpuStall()
{
	uint32_t cycles = _state.PendingCpuStallCycles;
	_state.PendingCpuStallCycles = 0;
	Exec(cycles);
}

template<typename T, bool isPeek>
T MdMemoryManager::InternalRead(uint32_t addr)
{
	uint8_t page = addr >> 16;
	T value;
	switch(_pageTypes[page]) {
		case MdPageType::Direct:
			value = ReadDirect<T>(_readPages[page], addr);
			break;

		case MdPageType::Cart: {
			bool handled;
			if constexpr(std::is_same<T, uint16_t>::value) {
				uint8_t hi = _cart->ReadRam(addr & ~0x01, handled);
				if(!handled) {
					hi = _readPages[page][addr & 0xFFFE];
				}
				uint8_t lo = _cart->ReadRam(addr | 0x01, handled);
				if(!handled) {
					lo = _readPages[page][(addr & 0xFFFE) + 1];
				}
				value = (hi << 8) | lo;
			} else {
				value = _cart->ReadRam(addr, handled);
				if(!handled) {
					value = _readPages[page][addr & 0xFFFF];
				}
			}
			break;
		}

		case MdPageType::Z80: value = ReadZ80Area<T, isPeek>(addr); break;
		case MdPageType::Io: value = ReadIo<T, isPeek>(addr); break;
		case MdPageType::Vdp: value = ReadVdpArea<T, isPeek>(addr); break;

		default:
		case MdPageType::OpenBus:
			value = GetOpenBus<T>(addr);
			break;
	}

	if constexpr(!isPeek) {
		if constexpr(std::is_same<T, uint16_t>::value) {
			_state.OpenBus = value;
		} else {
			if(addr & 0x01) {
				_state.OpenBus = (_state.OpenBus & 0xFF00) | value;
			} else {
				_state.OpenBus = (_state.OpenBus & 0x00FF) | (value << 8);
			}
		}
	}

	return value;
}

template<typename T>
void MdMemoryManager::InternalWrite(uint32_t addr, T value)
{
	uint8_t page = addr >> 16;
	switch(_pageTypes[page]) {
		case MdPageType::Direct:
			if(_writePages[page]) {
				WriteDirect<T>(_writePages[page], addr, value);
			}
			break;

		case MdPageType::Cart:
			if constexpr(std::is_same<T, uint16_t>::value) {
				_cart->WriteRam(addr & ~0x01, (uint8_t)(value >> 8));
				_cart->WriteRam(addr | 0x01, (uint8_t)value);
			} else {
				_cart->WriteRam(addr, value);
			}
			break;

		case MdPageType::Z80: WriteZ80Area<T>(addr, value); break;
		case MdPageType::Io: WriteIo<T>(addr, value); break;
		case MdPageType::Vdp: WriteVdpArea<T>(addr, value); break;

		default:
		case MdPageType::OpenBus:
			break;
	}
}

template<typename T, bool isPeek>
T MdMemoryManager::ReadZ80Area(uint32_t addr)
{
	if(!_z80->IsBusGranted()) {
		//TODOMD accessing the Z80's bus without owning it locks up the real hardware
		return GetOpenBus<T>(addr);
	}

	uint8_t value;
	uint16_t z80Addr = addr & 0x7FFF;
	if(z80Addr < 0x4000) {
		//Word reads return the byte at the even address in both halves
		value = _z80Ram[(std::is_same<T, uint16_t>::value ? (z80Addr & ~0x01) : z80Addr) & 0x1FFF];
	} else if(z80Addr < 0x6000) {
		if constexpr(isPeek) {
			value = 0;
		} else {
			value = _fmAudio->Read(z80Addr & 0x03);
		}
	} else {
		value = 0xFF;
	}

	if constexpr(std::is_same<T, uint16_t>::value) {
		return (value << 8) | value;
	} else {
		return value;
	}
}

template<typename T>
void MdMemoryManager::WriteZ80Area(uint32_t addr, T value)
{
	if(!_z80->IsBusGranted()) {
		return;
	}

	//Word writes only write the high byte (to the even address)
	uint8_t byteValue;
	uint16_t z80Addr;
	if constexpr(std::is_same<T, uint16_t>::value) {
		byteValue = (uint8_t)(value >> 8);
		z80Addr = addr & 0x7FFE;
	} else {
		byteValue = value;
		z80Addr = addr & 0x7FFF;
	}

	if(z80Addr < 0x4000) {
		_z80Ram[z80Addr & 0x1FFF] = byteValue;
	} else if(z80Addr < 0x6000) {
		_fmAudio->Write(z80Addr & 0x03, byteValue);
	} else if(z80Addr < 0x6100) {
		_z80->WriteBankRegister(byteValue);
	}
}

template<typename T, bool isPeek>
T MdMemoryManager::ReadIo(uint32_t addr)
{
	uint8_t value;
	if((addr & 0xFFFF) < 0x20) {
		uint8_t reg = (addr >> 1) & 0x0F;
		if constexpr(isPeek) {
			value = _controlManager->PeekRegister(reg);
		} else {
			value = _controlManager->ReadRegister(reg);
		}

		if constexpr(std::is_same<T, uint16_t>::value) {
			return (value << 8) | value;
		} else {
			return value;
		}
	}

	switch(addr & 0xFF00) {
		case 0x1100: {
			//Z80 BUSREQ: bit 0 (of the even byte) is 0 when the 68000 owns the Z80's bus
			uint8_t busy = _z80->IsBusGranted() ? 0 : 1;
			if constexpr(std::is_same<T, uint16_t>::value) {
				return (_state.OpenBus & 0xFEFF) | (busy << 8);
			} else {
				return (addr & 0x01) ? (uint8_t)_state.OpenBus : (uint8_t)(((_state.OpenBus >> 8) & 0xFE) | busy);
			}
		}

		default:
			return GetOpenBus<T>(addr);
	}
}

template<typename T>
void MdMemoryManager::WriteIo(uint32_t addr, T value)
{
	if((addr & 0xFFFF) < 0x20) {
		//I/O registers are on the low byte lane
		_controlManager->WriteRegister((addr >> 1) & 0x0F, (uint8_t)value);
		return;
	}

	//Control registers use bit 8 of word writes, or bit 0 of byte writes to the even address
	bool bitSet;
	bool evenLane;
	bool oddLane;
	if constexpr(std::is_same<T, uint16_t>::value) {
		bitSet = (value & 0x100) != 0;
		evenLane = true;
		oddLane = true;
	} else {
		evenLane = (addr & 0x01) == 0;
		oddLane = !evenLane;
		bitSet = evenLane && (value & 0x01) != 0;
	}

	switch(addr & 0xFF00) {
		case 0x1100:
			if(evenLane) {
				_z80->SetBusRequest(bitSet);
			}
			break;

		case 0x1200:
			if(evenLane) {
				bool reset = !bitSet;
				if(reset && !_z80->GetState().Reset) {
					//The YM2612's reset pin is tied to the Z80's reset line
					_fmAudio->Reset();
				}
				_z80->SetReset(reset);
			}
			break;

		case 0x3000:
			if(oddLane) {
				_cart->WriteRegister(addr, (uint8_t)value);
				RefreshMappings();
			}
			break;

		default:
			//TODOMD $A11000 (memory mode), $A14000 (TMSS) and $A14101 are not emulated
			break;
	}
}

template<typename T, bool isPeek>
T MdMemoryManager::ReadVdpArea(uint32_t addr)
{
	if((addr & 0xE700E0) != 0xC00000) {
		//TODOMD invalid VDP mirrors lock up the real hardware
		return GetOpenBus<T>(addr);
	}

	uint8_t port = addr & 0x1F;
	if(port >= 0x10) {
		//PSG, debug registers, etc. are not readable
		return GetOpenBus<T>(addr);
	}

	uint8_t vdpPort = port & 0x0C;
	uint16_t value;
	if constexpr(isPeek) {
		value = _vdp->PeekPort(vdpPort);
	} else {
		if(!_inZ80Access) {
			while(_vdp->IsCpuReadBlocked(vdpPort)) {
				Exec(1);
			}
		}
		value = _vdp->ReadPort(vdpPort);
	}

	if(vdpPort == 0x04) {
		//The upper 6 bits of the status register are open bus
		value = (value & 0x03FF) | (_state.OpenBus & 0xFC00);
	}

	if constexpr(std::is_same<T, uint16_t>::value) {
		return value;
	} else {
		return (addr & 0x01) ? (uint8_t)value : (uint8_t)(value >> 8);
	}
}

template<typename T>
void MdMemoryManager::WriteVdpArea(uint32_t addr, T value)
{
	if((addr & 0xE700E0) != 0xC00000) {
		return;
	}

	uint8_t port = addr & 0x1F;
	if(port >= 0x10) {
		if(port < 0x18) {
			//PSG is on the low byte lane
			if constexpr(std::is_same<T, uint16_t>::value) {
				_psg->Write((uint8_t)value);
			} else if(addr & 0x01) {
				_psg->Write(value);
			}
		}
		return;
	}

	uint8_t vdpPort = port & 0x0C;
	if(vdpPort >= 0x08) {
		//HV counter is read-only
		return;
	}

	uint16_t wordValue;
	if constexpr(std::is_same<T, uint16_t>::value) {
		wordValue = value;
	} else {
		//Byte writes are written to both halves of the bus
		wordValue = (value << 8) | value;
	}

	if(!_inZ80Access) {
		while(_vdp->IsCpuWriteBlocked(vdpPort)) {
			Exec(1);
		}
	}

	_vdp->WritePort(vdpPort, wordValue);

	if(!_inZ80Access) {
		while(_vdp->IsCpuBusLocked()) {
			//The 68000 is stopped while a 68000->VDP DMA transfer runs
			Exec(1);
		}
	}
}

void MdMemoryManager::WriteTas(uint32_t addr, uint8_t value)
{
	//The bus arbiter doesn't support the 68000's read-modify-write cycle, so TAS's write never reaches memory
	//The VDP and other devices outside of the cart/work RAM still see the write
	addr &= 0xFFFFFF;
	ProcessRefresh();
	Exec(2);
	uint8_t page = addr >> 16;
	if(_pageTypes[page] != MdPageType::Direct && _pageTypes[page] != MdPageType::Cart) {
		if(_emu->ProcessMemoryWrite<CpuType::Md, 1>(addr, value, MemoryOperationType::Write)) {
			InternalWrite<uint8_t>(addr, value);
		}
	}
	Exec(2);
}

uint8_t MdMemoryManager::AcknowledgeInterrupt(uint8_t level)
{
	_vdp->AcknowledgeInterrupt(level);

	//All interrupts use autovectors on the Mega Drive
	return 24 + level;
}

void MdMemoryManager::ProcessResetInstruction()
{
	//The 68000's reset output isn't connected to anything relevant on the Mega Drive
}

uint16_t MdMemoryManager::ReadDma(uint32_t addr)
{
	addr &= 0xFFFFFE;
	uint8_t page = addr >> 16;
	switch(_pageTypes[page]) {
		case MdPageType::Direct:
		case MdPageType::Cart:
			return InternalRead<uint16_t, true>(addr);

		default:
			//TODOMD DMA from other areas (e.g Z80 RAM) returns garbage on real hardware
			return 0xFFFF;
	}
}

uint8_t MdMemoryManager::ReadZ80Bank(uint16_t addr)
{
	uint32_t cpuAddr = _z80->GetBankAddress() | (addr & 0x7FFF);

	_z80->AddWaitCycles(MdMemoryManager::Z80BankAccessZ80Wait);
	_state.PendingCpuStallCycles += MdMemoryManager::Z80BankAccessCpuStall;

	if((cpuAddr >> 16) == 0xA0) {
		//TODOMD the Z80 accessing its own address space through the bank window locks up the real hardware
		return 0xFF;
	}

	_inZ80Access = true;
	uint8_t value = InternalRead<uint8_t>(cpuAddr);
	_inZ80Access = false;
	return value;
}

void MdMemoryManager::WriteZ80Bank(uint16_t addr, uint8_t value)
{
	uint32_t cpuAddr = _z80->GetBankAddress() | (addr & 0x7FFF);

	_z80->AddWaitCycles(MdMemoryManager::Z80BankAccessZ80Wait);
	_state.PendingCpuStallCycles += MdMemoryManager::Z80BankAccessCpuStall;

	if((cpuAddr >> 16) == 0xA0) {
		return;
	}

	_inZ80Access = true;
	InternalWrite<uint8_t>(cpuAddr, value);
	_inZ80Access = false;
}

uint8_t MdMemoryManager::ReadZ80(uint16_t addr, MemoryOperationType opType)
{
	uint8_t value;
	switch(addr >> 13) {
		case 0:
		case 1:
			value = _z80Ram[addr & 0x1FFF];
			break;

		case 2:
			value = _fmAudio->Read(addr & 0x03);
			break;

		case 3:
			if(addr >= 0x7F00 && addr < 0x7F10) {
				_inZ80Access = true;
				value = ReadVdpArea<uint8_t, false>(0xC00000 | (addr & 0x1F));
				_inZ80Access = false;
			} else {
				value = 0xFF;
			}
			break;

		default:
			value = ReadZ80Bank(addr);
			break;
	}

	_emu->ProcessMemoryRead<CpuType::MdZ80>(addr, value, opType);
	return value;
}

void MdMemoryManager::WriteZ80(uint16_t addr, uint8_t value, MemoryOperationType opType)
{
	if(!_emu->ProcessMemoryWrite<CpuType::MdZ80>(addr, value, opType)) {
		return;
	}

	switch(addr >> 13) {
		case 0:
		case 1:
			_z80Ram[addr & 0x1FFF] = value;
			break;

		case 2:
			_fmAudio->Write(addr & 0x03, value);
			break;

		case 3:
			if(addr < 0x6100) {
				_z80->WriteBankRegister(value);
			} else if(addr >= 0x7F00 && addr < 0x7F20) {
				_inZ80Access = true;
				WriteVdpArea<uint8_t>(0xC00000 | (addr & 0x1F), value);
				_inZ80Access = false;
			}
			break;

		default:
			WriteZ80Bank(addr, value);
			break;
	}
}

uint8_t MdMemoryManager::ReadZ80Port(uint8_t port)
{
	//The Z80's I/O ports are not connected to anything
	return 0xFF;
}

void MdMemoryManager::WriteZ80Port(uint8_t port, uint8_t value)
{
}

uint8_t MdMemoryManager::DebugRead(uint32_t addr)
{
	return InternalRead<uint8_t, true>(addr & 0xFFFFFF);
}

uint16_t MdMemoryManager::DebugReadWord(uint32_t addr)
{
	return InternalRead<uint16_t, true>(addr & 0xFFFFFE);
}

void MdMemoryManager::DebugWrite(uint32_t addr, uint8_t value)
{
	AddressInfo absAddr = GetAbsoluteAddress(addr);
	if(absAddr.Address >= 0) {
		ConsoleMemoryInfo mem = _emu->GetMemory(absAddr.Type);
		if((uint32_t)absAddr.Address < mem.Size) {
			((uint8_t*)mem.Memory)[absAddr.Address] = value;
		}
	}
}

uint8_t MdMemoryManager::DebugReadZ80(uint16_t addr)
{
	switch(addr >> 13) {
		case 0:
		case 1:
			return _z80Ram[addr & 0x1FFF];

		case 2:
		case 3:
			return 0xFF;

		default: {
			uint32_t cpuAddr = _z80->GetBankAddress() | (addr & 0x7FFF);
			return (cpuAddr >> 16) == 0xA0 ? 0xFF : DebugRead(cpuAddr);
		}
	}
}

void MdMemoryManager::DebugWriteZ80(uint16_t addr, uint8_t value)
{
	if(addr < 0x4000) {
		_z80Ram[addr & 0x1FFF] = value;
	} else if(addr >= 0x8000) {
		uint32_t cpuAddr = _z80->GetBankAddress() | (addr & 0x7FFF);
		if((cpuAddr >> 16) != 0xA0) {
			DebugWrite(cpuAddr, value);
		}
	}
}

AddressInfo MdMemoryManager::GetAbsoluteAddress(uint32_t addr)
{
	addr &= 0xFFFFFF;
	uint8_t page = addr >> 16;
	switch(_pageTypes[page]) {
		case MdPageType::Direct:
		case MdPageType::Cart:
			if(page >= 0xE0) {
				return { (int32_t)(addr & 0xFFFF), MemoryType::MdWorkRam };
			}
			return { (int32_t)((_readPages[page] - _cart->GetPrgRom()) + (addr & 0xFFFF)), MemoryType::MdPrgRom };

		case MdPageType::Z80:
			if((addr & 0x7FFF) < 0x4000) {
				return { (int32_t)(addr & 0x1FFF), MemoryType::MdZ80Ram };
			}
			break;

		default:
			break;
	}

	return { -1, MemoryType::None };
}

int32_t MdMemoryManager::GetRelativeAddress(AddressInfo& absAddress)
{
	switch(absAddress.Type) {
		case MemoryType::MdWorkRam:
			return 0xFF0000 | (absAddress.Address & 0xFFFF);

		case MemoryType::MdZ80Ram:
			return 0xA00000 | (absAddress.Address & 0x1FFF);

		case MemoryType::MdPrgRom:
			for(int i = 0; i < 0x40; i++) {
				int32_t offset = (int32_t)(_readPages[i] - _cart->GetPrgRom());
				if(absAddress.Address >= offset && absAddress.Address < offset + 0x10000) {
					return (i << 16) | (absAddress.Address - offset);
				}
			}
			return -1;

		default:
			return -1;
	}
}

AddressInfo MdMemoryManager::GetZ80AbsoluteAddress(uint16_t addr)
{
	if(addr < 0x4000) {
		return { (int32_t)(addr & 0x1FFF), MemoryType::MdZ80Ram };
	} else if(addr >= 0x8000) {
		return GetAbsoluteAddress(_z80->GetBankAddress() | (addr & 0x7FFF));
	}
	return { -1, MemoryType::None };
}

int32_t MdMemoryManager::GetZ80RelativeAddress(AddressInfo& absAddress)
{
	if(absAddress.Type == MemoryType::MdZ80Ram) {
		return absAddress.Address & 0x1FFF;
	}

	int32_t cpuAddr = GetRelativeAddress(absAddress);
	if(cpuAddr >= 0 && ((uint32_t)cpuAddr & 0xFF8000) == _z80->GetBankAddress()) {
		return 0x8000 | (cpuAddr & 0x7FFF);
	}
	return -1;
}

void MdMemoryManager::Serialize(Serializer& s)
{
	SVArray(_workRam, MdConstants::WorkRamSize);
	SVArray(_z80Ram, MdConstants::Z80RamSize);

	SV(_state.OpenBus);
	SV(_state.PendingCpuStallCycles);
	SV(_masterClock);
	SV(_lastRefreshCycle);

	if(!s.IsSaving()) {
		RefreshMappings();
	}
}

template uint8_t MdMemoryManager::InternalRead<uint8_t, false>(uint32_t addr);
template uint16_t MdMemoryManager::InternalRead<uint16_t, false>(uint32_t addr);
template void MdMemoryManager::InternalWrite<uint8_t>(uint32_t addr, uint8_t value);
template void MdMemoryManager::InternalWrite<uint16_t>(uint32_t addr, uint16_t value);
