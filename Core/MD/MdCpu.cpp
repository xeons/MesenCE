#include "pch.h"
#include "MD/MdCpu.h"
#include "MD/MdTypes.h"
#include "MD/MdMemoryManager.h"
#include "Shared/Emulator.h"
#include "Utilities/Serializer.h"

MdCpu::OpFunc MdCpu::_opTable[0x10000] = {};
bool MdCpu::_opTableInitialized = false;

void MdCpu::Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager)
{
	_emu = emu;
	_console = console;
	_memoryManager = memoryManager;

	static bool initDone = (InitOpTable(), true);
	(void)initDone;

	_state = {};
	_state.SR = 0x2700;
}

void MdCpu::Reset()
{
	_state.Stopped = false;
	_state.Halted = false;
	_inException = false;

	SetSR(0x2700 | (_state.SR & 0x1F));

	Idle(16);
	_state.SSP = ReadLong(0);
	_state.A[7] = _state.SSP;
	uint32_t pc = ReadLong(4);
	FillQueue(pc);

#ifndef DUMMYCPU
	_state.CycleCount = _memoryManager->GetMasterClock() / MdConstants::CpuDivider;
#endif
}

void MdCpu::Exec()
{
	if(_state.Halted) {
		Idle(4);
	} else {
		uint8_t irqLevel = _state.PendingIrqLevel;
		bool nmi = irqLevel == 7 && _state.NmiEdge;
		if(irqLevel > GetIrqMask() || nmi) {
			try {
				if(nmi) {
					_state.NmiEdge = false;
				}
				ProcessInterrupt(irqLevel);
			} catch(AddressErrorException&) {
				_inException = false;
				_state.Halted = true;
			}
		} else if(_state.Stopped) {
			Idle(4);
			_state.PendingIrqLevel = _state.IrqLevel;
		} else {
			bool trace = (_state.SR & MdCpuFlags::Trace) != 0;
#ifndef DUMMYCPU
			_emu->ProcessInstruction<CpuType::Md>();
#endif
			try {
				_opCode = _state.IR;
				_state.IRD = _opCode;
				_skipTrace = false;
				(this->*_opTable[_opCode])();
				if(trace && !_skipTrace) {
					ProcessTrace();
				}
			} catch(AddressErrorException&) {
				try {
					ProcessAddressError();
				} catch(AddressErrorException&) {
					_inException = false;
					_state.Halted = true;
				}
			}
		}
	}

#ifndef DUMMYCPU
	_state.CycleCount = _memoryManager->GetMasterClock() / MdConstants::CpuDivider;
#endif
}

void MdCpu::SetIrqSource(MdIrqSource source)
{
	_state.ActiveIrqs |= (1 << (int)source);
	UpdateIrqLevel();
}

void MdCpu::ClearIrqSource(MdIrqSource source)
{
	_state.ActiveIrqs &= ~(1 << (int)source);
	UpdateIrqLevel();
}

void MdCpu::UpdateIrqLevel()
{
	uint8_t level = 0;
	for(int i = 7; i > 0; i--) {
		if(_state.ActiveIrqs & (1 << i)) {
			level = i;
			break;
		}
	}

	if(level == 7 && _state.IrqLevel != 7) {
		_state.NmiEdge = true;
	}
	_state.IrqLevel = level;

	if(_state.Stopped) {
		_state.PendingIrqLevel = level;
	}
}

MdCpuState& MdCpu::GetState()
{
	if(IsSupervisor()) {
		_state.SSP = _state.A[7];
	} else {
		_state.USP = _state.A[7];
	}
	return _state;
}

void MdCpu::Idle(uint32_t cycles)
{
#ifndef DUMMYCPU
	_memoryManager->Exec(cycles);
#endif
}

uint8_t MdCpu::BusReadByte(uint32_t addr, MemoryOperationType type)
{
#ifdef DUMMYCPU
	uint8_t value = _memoryManager->DebugRead(addr & 0xFFFFFF);
	LogMemoryOperation(addr & 0xFFFFFF, value, type, MemoryType::MdMemory);
	return value;
#else
	_state.PendingIrqLevel = _state.IrqLevel;
	return _memoryManager->Read<uint8_t>(addr, type);
#endif
}

uint16_t MdCpu::BusReadWord(uint32_t addr, MemoryOperationType type)
{
#ifdef DUMMYCPU
	uint16_t value = _memoryManager->DebugReadWord(addr & 0xFFFFFF);
	LogMemoryOperation(addr & 0xFFFFFF, value, type, MemoryType::MdMemory);
	return value;
#else
	_state.PendingIrqLevel = _state.IrqLevel;
	return _memoryManager->Read<uint16_t>(addr, type);
#endif
}

void MdCpu::BusWriteByte(uint32_t addr, uint8_t value)
{
#ifdef DUMMYCPU
	LogMemoryOperation(addr & 0xFFFFFF, value, MemoryOperationType::Write, MemoryType::MdMemory);
#else
	_state.PendingIrqLevel = _state.IrqLevel;
	_memoryManager->Write<uint8_t>(addr, value, MemoryOperationType::Write);
#endif
}

void MdCpu::BusWriteWord(uint32_t addr, uint16_t value)
{
#ifdef DUMMYCPU
	LogMemoryOperation(addr & 0xFFFFFF, value, MemoryOperationType::Write, MemoryType::MdMemory);
#else
	_state.PendingIrqLevel = _state.IrqLevel;
	_memoryManager->Write<uint16_t>(addr, value, MemoryOperationType::Write);
#endif
}

void MdCpu::AddressError(uint32_t addr, bool isRead, bool isProgram)
{
	uint8_t fc = (IsSupervisor() ? 4 : 0) | (isProgram ? 2 : 1);
	_faultAddress = addr;
	_faultInfo = (_state.IRD & 0xFFE0) | (isRead ? 0x10 : 0) | ((isProgram || _inException) ? 0x08 : 0) | fc;
	_faultPc = _state.PC;
	throw AddressErrorException();
}

uint8_t MdCpu::ReadByte(uint32_t addr)
{
	return BusReadByte(addr, MemoryOperationType::Read);
}

uint16_t MdCpu::ReadWord(uint32_t addr)
{
	if(addr & 0x01) {
		AddressError(addr, true, false);
	}
	return BusReadWord(addr, MemoryOperationType::Read);
}

uint32_t MdCpu::ReadLong(uint32_t addr)
{
	if(addr & 0x01) {
		AddressError(addr, true, false);
	}
	uint32_t hi = BusReadWord(addr, MemoryOperationType::Read);
	uint32_t lo = BusReadWord(addr + 2, MemoryOperationType::Read);
	return (hi << 16) | lo;
}

void MdCpu::WriteByte(uint32_t addr, uint8_t value)
{
	BusWriteByte(addr, value);
}

void MdCpu::WriteWord(uint32_t addr, uint16_t value)
{
	if(addr & 0x01) {
		AddressError(addr, false, false);
	}
	BusWriteWord(addr, value);
}

void MdCpu::WriteLong(uint32_t addr, uint32_t value)
{
	if(addr & 0x01) {
		AddressError(addr, false, false);
	}
	BusWriteWord(addr, (uint16_t)(value >> 16));
	BusWriteWord(addr + 2, (uint16_t)value);
}

void MdCpu::WriteLongReversed(uint32_t addr, uint32_t value)
{
	if(addr & 0x01) {
		AddressError(addr, false, false);
	}
	BusWriteWord(addr + 2, (uint16_t)value);
	BusWriteWord(addr, (uint16_t)(value >> 16));
}

uint16_t MdCpu::ReadProgramWord(uint32_t addr)
{
	return BusReadWord(addr, MemoryOperationType::ExecOperand);
}

uint16_t MdCpu::ReadExt()
{
	uint16_t value = _state.IRC;
	_state.PC += 2;
	_state.IRC = ReadProgramWord(_state.PC + 2);
	return value;
}

void MdCpu::Prefetch()
{
	_state.PC += 2;
	_state.IR = _state.IRC;
	_state.IRC = BusReadWord(_state.PC + 2, MemoryOperationType::ExecOpCode);
}

void MdCpu::FillQueue(uint32_t addr, uint8_t idleCycles)
{
	if(addr & 0x01) {
		_state.PC = addr - 4;
		AddressError(addr, true, true);
	}
	_state.PC = addr;
	_state.IR = BusReadWord(addr, MemoryOperationType::ExecOpCode);
	if(idleCycles) {
		Idle(idleCycles);
	}
	_state.IRC = ReadProgramWord(addr + 2);
}

void MdCpu::SetSR(uint16_t value)
{
	value &= 0xA71F;
	bool wasSupervisor = (_state.SR & MdCpuFlags::Supervisor) != 0;
	bool isSupervisor = (value & MdCpuFlags::Supervisor) != 0;
	if(wasSupervisor != isSupervisor) {
		if(wasSupervisor) {
			_state.SSP = _state.A[7];
			_state.A[7] = _state.USP;
		} else {
			_state.USP = _state.A[7];
			_state.A[7] = _state.SSP;
		}
	}
	_state.SR = value;
}

bool MdCpu::CheckCondition(uint8_t cond)
{
	uint16_t sr = _state.SR;
	bool c = sr & MdCpuFlags::Carry;
	bool v = sr & MdCpuFlags::Overflow;
	bool z = sr & MdCpuFlags::Zero;
	bool n = sr & MdCpuFlags::Negative;

	switch(cond & 0x0F) {
		default:
		case 0: return true;
		case 1: return false;
		case 2: return !c && !z;
		case 3: return c || z;
		case 4: return !c;
		case 5: return c;
		case 6: return !z;
		case 7: return z;
		case 8: return !v;
		case 9: return v;
		case 10: return !n;
		case 11: return n;
		case 12: return n == v;
		case 13: return n != v;
		case 14: return !z && n == v;
		case 15: return z || n != v;
	}
}

void MdCpu::ProcessException(uint8_t vector, uint32_t pcToPush, uint8_t preIdleCycles)
{
	_inException = true;
	uint16_t sr = _state.SR;
	SetSR((sr | MdCpuFlags::Supervisor) & ~MdCpuFlags::Trace);

	if(preIdleCycles) {
		Idle(preIdleCycles);
	}

	uint32_t sp = _state.A[7];
	WriteWord(sp - 2, (uint16_t)pcToPush);
	WriteWord(sp - 6, sr);
	WriteWord(sp - 4, (uint16_t)(pcToPush >> 16));
	_state.A[7] = sp - 6;

	uint32_t newPc = ReadLong(vector * 4);
	_inException = false;
	FillQueue(newPc, 2);
}

void MdCpu::ProcessAddressError()
{
	_inException = true;
	uint16_t sr = _state.SR;
	SetSR((sr | MdCpuFlags::Supervisor) & ~MdCpuFlags::Trace);

	Idle(4);

	uint32_t sp = _state.A[7];
	WriteWord(sp - 2, (uint16_t)_faultPc);
	WriteWord(sp - 6, sr);
	WriteWord(sp - 4, (uint16_t)(_faultPc >> 16));
	WriteWord(sp - 8, _state.IRD);
	WriteWord(sp - 10, (uint16_t)_faultAddress);
	WriteWord(sp - 14, _faultInfo);
	WriteWord(sp - 12, (uint16_t)(_faultAddress >> 16));
	_state.A[7] = sp - 14;

	uint32_t newPc = ReadLong(3 * 4);
	_inException = false;
	FillQueue(newPc, 2);
}

void MdCpu::ProcessInterrupt(uint8_t level)
{
#ifndef DUMMYCPU
	uint32_t originalPc = _state.PC;
#endif

	_state.Stopped = false;
	_inException = true;

	uint16_t sr = _state.SR;
	SetSR(((sr | MdCpuFlags::Supervisor) & ~(MdCpuFlags::Trace | MdCpuFlags::IrqMask)) | (level << 8));

	Idle(6);
	uint32_t sp = _state.A[7];
	WriteWord(sp - 2, (uint16_t)_state.PC);

	//Interrupt acknowledge cycle (autovector, synchronized with the E clock)
	uint8_t vector = _memoryManager->AcknowledgeInterrupt(level);
	Idle(8);

	WriteWord(sp - 6, sr);
	WriteWord(sp - 4, (uint16_t)(_state.PC >> 16));
	_state.A[7] = sp - 6;

	uint32_t newPc = ReadLong(vector * 4);
	_inException = false;
	FillQueue(newPc, 2);

#ifndef DUMMYCPU
	_emu->ProcessInterrupt<CpuType::Md>(originalPc, _state.PC, false);
#endif
}

void MdCpu::ProcessTrace()
{
	ProcessException(9, _state.PC, 4);
}

void MdCpu::PrivilegeViolation()
{
	_skipTrace = true;
	ProcessException(8, _state.PC, 4);
}

bool MdCpu::CheckPrivilege()
{
	if(!IsSupervisor()) {
		PrivilegeViolation();
		return false;
	}
	return true;
}

uint32_t MdCpu::GetIndexedAddress(uint32_t base)
{
	uint16_t ext = ReadExt();
	uint8_t reg = (ext >> 12) & 0x07;
	uint32_t index = (ext & 0x8000) ? _state.A[reg] : _state.D[reg];
	if(!(ext & 0x800)) {
		index = (uint32_t)(int16_t)index;
	}
	return base + (int8_t)ext + index;
}

bool MdCpu::IsValidEa(uint8_t mode, uint8_t reg, uint16_t allowedModes)
{
	int index = mode < 7 ? mode : (reg < 5 ? 7 + reg : 12);
	return index < 12 && (allowedModes & (1 << index)) != 0;
}

void MdCpu::Serialize(Serializer& s)
{
	SV(_state.CycleCount);
	SVArray(_state.D, 8);
	SVArray(_state.A, 8);
	SV(_state.PC);
	SV(_state.USP);
	SV(_state.SSP);
	SV(_state.SR);
	SV(_state.IR);
	SV(_state.IRC);
	SV(_state.IRD);
	SV(_state.ActiveIrqs);
	SV(_state.IrqLevel);
	SV(_state.PendingIrqLevel);
	SV(_state.NmiEdge);
	SV(_state.Stopped);
	SV(_state.Halted);
}

#ifdef DUMMYCPU
void MdCpu::SetDummyState(MdCpuState& state)
{
	_memOpCounter = 0;
	_state = state;
}

uint32_t MdCpu::GetOperationCount()
{
	return _memOpCounter;
}

void MdCpu::LogMemoryOperation(uint32_t addr, uint16_t value, MemoryOperationType type, MemoryType memType)
{
	if(_memOpCounter < 32) {
		_memOperations[_memOpCounter].Address = addr;
		_memOperations[_memOpCounter].Value = value;
		_memOperations[_memOpCounter].Type = type;
		_memOperations[_memOpCounter].MemType = memType;
		_memOpCounter++;
	}
}

MemoryOperationInfo MdCpu::GetOperationInfo(uint32_t index)
{
	return _memOperations[index];
}
#endif

