#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "MD/MdVdp.h"
#include "MD/MdZ80.h"
#include "Shared/Emulator.h"
#include "Shared/CheatManager.h"
#include "Debugger/AddressInfo.h"
#include "Shared/MemoryOperationType.h"
#include "Utilities/ISerializable.h"

class MdConsole;
class MdCpu;
class MdCart;
class MdPsg;
class MdFmAudio;
class MdControlManager;

class MdMemoryManager final : public ISerializable
{
private:
	//The 68000's DRAM refresh steals ~2 cycles every 128 cycles (approximation of the real behavior)
	static constexpr uint32_t RefreshInterval = 128;
	static constexpr uint32_t RefreshDelay = 2;

	enum class MdPageType : uint8_t
	{
		OpenBus,
		Direct, //ROM or work RAM - read/written directly via _readPages/_writePages
		Cart, //Cart area with special handling (cart RAM, mapper registers, etc.)
		Z80,
		Io,
		Vdp,
	};

	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdCpu* _cpu = nullptr;
	MdZ80Cpu* _z80 = nullptr;
	MdVdp* _vdp = nullptr;
	MdCart* _cart = nullptr;
	MdPsg* _psg = nullptr;
	MdFmAudio* _fmAudio = nullptr;
	MdControlManager* _controlManager = nullptr;

	//Z80 bank accesses stall the 68000 while the Z80 uses its bus (approximation)
	static constexpr uint32_t Z80BankAccessCpuStall = 6;
	static constexpr uint8_t Z80BankAccessZ80Wait = 3;

	MdMemoryManagerState _state = {};
	uint64_t _masterClock = 0;
	uint64_t _lastRefreshCycle = 0;

	//Set while the Z80 is accessing the 68000's bus (the Z80 can't be stalled by VDP waits like the 68000)
	bool _inZ80Access = false;

	uint8_t* _workRam = nullptr;
	uint8_t* _z80Ram = nullptr;

	//68000 address space, in 64KB pages (memory is stored in big endian order, like the ROM itself)
	uint8_t* _readPages[0x100] = {};
	uint8_t* _writePages[0x100] = {};
	MdPageType _pageTypes[0x100] = {};

	template<typename T> __forceinline T ReadDirect(uint8_t* page, uint32_t addr)
	{
		if constexpr(std::is_same<T, uint16_t>::value) {
			return (page[addr & 0xFFFE] << 8) | page[(addr & 0xFFFE) + 1];
		} else {
			return page[addr & 0xFFFF];
		}
	}

	template<typename T> __forceinline void WriteDirect(uint8_t* page, uint32_t addr, T value)
	{
		if constexpr(std::is_same<T, uint16_t>::value) {
			page[addr & 0xFFFE] = (uint8_t)(value >> 8);
			page[(addr & 0xFFFE) + 1] = (uint8_t)value;
		} else {
			page[addr & 0xFFFF] = value;
		}
	}

	template<typename T, bool isPeek = false> T InternalRead(uint32_t addr);
	template<typename T> void InternalWrite(uint32_t addr, T value);

	template<typename T, bool isPeek> T ReadZ80Area(uint32_t addr);
	template<typename T> void WriteZ80Area(uint32_t addr, T value);

	template<typename T, bool isPeek> T ReadIo(uint32_t addr);
	template<typename T> void WriteIo(uint32_t addr, T value);

	template<typename T, bool isPeek> T ReadVdpArea(uint32_t addr);
	template<typename T> void WriteVdpArea(uint32_t addr, T value);

	template<typename T> __forceinline T GetOpenBus(uint32_t addr)
	{
		if constexpr(std::is_same<T, uint16_t>::value) {
			return _state.OpenBus;
		} else {
			return (addr & 0x01) ? (uint8_t)_state.OpenBus : (uint8_t)(_state.OpenBus >> 8);
		}
	}

	uint8_t ReadZ80Bank(uint16_t addr);
	void WriteZ80Bank(uint16_t addr, uint8_t value);

	__forceinline void ProcessRefresh()
	{
		uint64_t cpuCycle = _masterClock / MdConstants::CpuDivider;
		if(cpuCycle - _lastRefreshCycle >= MdMemoryManager::RefreshInterval) {
			_lastRefreshCycle = cpuCycle;
			Exec(MdMemoryManager::RefreshDelay);
		}
	}

	void ProcessCpuStall();

public:
	MdMemoryManager();
	~MdMemoryManager();

	void Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80Cpu* z80, MdVdp* vdp, MdCart* cart, MdPsg* psg, MdFmAudio* fmAudio, MdControlManager* controlManager);

	MdMemoryManagerState& GetState() { return _state; }
	uint64_t GetMasterClock() { return _masterClock; }

	//Advances emulation by the given number of 68000 cycles (VDP and Z80 catch up to the new master clock)
	__forceinline void Exec(uint32_t cpuCycles)
	{
		_masterClock += cpuCycles * MdConstants::CpuDivider;
		_vdp->Run(_masterClock);
		_z80->Run(_masterClock);
		if(_state.PendingCpuStallCycles) {
			ProcessCpuStall();
		}
	}

	void RefreshMappings();

	//68000 bus - performs a full bus cycle, including wait states
	template<typename T>
	__forceinline T Read(uint32_t addr, MemoryOperationType opType = MemoryOperationType::Read)
	{
		addr &= 0xFFFFFF;
		ProcessRefresh();
		Exec(2);
		T value = InternalRead<T>(addr);
		Exec(2);

		if(_emu->GetCheatManager()->HasCheats<CpuType::Md>()) {
			if constexpr(std::is_same<T, uint16_t>::value) {
				uint8_t hi = (uint8_t)(value >> 8);
				uint8_t lo = (uint8_t)value;
				_emu->GetCheatManager()->ApplyCheat<CpuType::Md>(addr & ~0x01, hi);
				_emu->GetCheatManager()->ApplyCheat<CpuType::Md>(addr | 0x01, lo);
				value = (hi << 8) | lo;
			} else {
				_emu->GetCheatManager()->ApplyCheat<CpuType::Md>(addr, value);
			}
		}

		_emu->ProcessMemoryRead<CpuType::Md, sizeof(T)>(addr, value, opType);
		return value;
	}

	template<typename T>
	__forceinline void Write(uint32_t addr, T value, MemoryOperationType opType = MemoryOperationType::Write)
	{
		addr &= 0xFFFFFF;
		ProcessRefresh();
		Exec(2);
		if(_emu->ProcessMemoryWrite<CpuType::Md, sizeof(T)>(addr, value, opType)) {
			InternalWrite<T>(addr, value);
		}
		Exec(2);
	}

	//TAS's write cycle is ignored on the Mega Drive (the bus arbiter does not support read-modify-write cycles)
	void WriteTas(uint32_t addr, uint8_t value);

	//Interrupt acknowledge cycle - returns the vector number to use (the Mega Drive always uses autovectors)
	uint8_t AcknowledgeInterrupt(uint8_t level);

	//Called when the 68000 executes the RESET instruction
	void ProcessResetInstruction();

	//Used by the VDP's 68000->VDP DMA transfers (no timing - the 68000 is stopped while DMA runs)
	uint16_t ReadDma(uint32_t addr);

	//Z80 bus
	uint8_t ReadZ80(uint16_t addr, MemoryOperationType opType = MemoryOperationType::Read);
	void WriteZ80(uint16_t addr, uint8_t value, MemoryOperationType opType = MemoryOperationType::Write);
	uint8_t ReadZ80Port(uint8_t port);
	void WriteZ80Port(uint8_t port, uint8_t value);

	//Debugger/cheat access (no side effects)
	uint8_t DebugRead(uint32_t addr);
	uint16_t DebugReadWord(uint32_t addr);
	void DebugWrite(uint32_t addr, uint8_t value);
	uint8_t DebugReadZ80(uint16_t addr);
	void DebugWriteZ80(uint16_t addr, uint8_t value);

	AddressInfo GetAbsoluteAddress(uint32_t addr);
	int32_t GetRelativeAddress(AddressInfo& absAddress);
	AddressInfo GetZ80AbsoluteAddress(uint16_t addr);
	int32_t GetZ80RelativeAddress(AddressInfo& absAddress);

	void Serialize(Serializer& s) override;
};
