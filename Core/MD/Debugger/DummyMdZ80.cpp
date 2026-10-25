#include "pch.h"
#include "MD/Debugger/DummyMdZ80.h"

#define DUMMYCPU
#define MdZ80Cpu DummyMdZ80
#include "MD/MdZ80.cpp"
#undef MdZ80Cpu
#undef DUMMYCPU

void DummyMdZ80::SetDummyState(MdZ80State& state)
{
	_memOpCounter = 0;
	_state = state;
	_state.Halted = false;
	_state.NmiLevel = false;
	_state.NmiPending = false;
	_state.ActiveIrqs = 0;
	_state.BusRequested = false;
	_state.Reset = false;
}

uint32_t DummyMdZ80::GetOperationCount()
{
	return _memOpCounter;
}

void DummyMdZ80::LogMemoryOperation(uint32_t addr, uint8_t value, MemoryOperationType type, MemoryType memType)
{
	_memOperations[_memOpCounter] = {
		addr,
		(int32_t)value,
		type,
		memType
	};
	_memOpCounter++;
}

MemoryOperationInfo DummyMdZ80::GetOperationInfo(uint32_t index)
{
	return _memOperations[index];
}
