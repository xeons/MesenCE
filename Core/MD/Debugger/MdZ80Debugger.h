#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/IDebugger.h"

class Disassembler;
class Debugger;
class MdZ80TraceLogger;
class MdConsole;
class MdMemoryManager;
class CallstackManager;
class MemoryAccessCounter;
class BreakpointManager;
class BaseEventManager;
class EmuSettings;
class SmsAssembler;
class Emulator;
class MdZ80Cpu;
class MdVdp;
class DummyMdZ80;

enum class MemoryOperationType;

class MdZ80Debugger final : public IDebugger
{
	Debugger* _debugger;
	Emulator* _emu;
	MdZ80Cpu* _cpu;
	MdVdp* _vdp;
	MdMemoryManager* _memoryManager;
	Disassembler* _disassembler;
	MemoryAccessCounter* _memoryAccessCounter;
	MdConsole* _console;
	EmuSettings* _settings;
	BaseEventManager* _eventManager;

	unique_ptr<CallstackManager> _callstackManager;
	unique_ptr<BreakpointManager> _breakpointManager;
	unique_ptr<SmsAssembler> _assembler;
	unique_ptr<MdZ80TraceLogger> _traceLogger;
	unique_ptr<DummyMdZ80> _dummyCpu;

	uint16_t _prevOpCode = 0xFF;
	uint16_t _prevStackPointer = 0;
	uint32_t _prevProgramCounter = 0;

	__forceinline uint8_t GetPrevOpCodeSize();
	__forceinline void ProcessCallStackUpdates(AddressInfo& destAddr, uint16_t destPc, uint16_t sp);
	void ProcessRegisterAccess(MemoryOperationInfo& operation);

public:
	MdZ80Debugger(Debugger* debugger);
	~MdZ80Debugger();

	void Reset() override;

	void ProcessInstruction();
	void ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type);
	void ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type);

	void ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi) override;

	void Run() override;
	void Step(int32_t stepCount, StepType type) override;

	bool SaveRomToDisk(string filename, bool saveAsIps, CdlStripOption stripOption);

	void SetProgramCounter(uint32_t addr, bool updateDebuggerOnly = false) override;
	uint32_t GetProgramCounter(bool getInstPc) override;
	uint64_t GetCpuCycleCount(bool forProfiler) override;
	void ResetPrevOpCode() override;

	DebuggerFeatures GetSupportedFeatures() override;

	BaseEventManager* GetEventManager() override;
	IAssembler* GetAssembler() override;
	CallstackManager* GetCallstackManager() override;
	BreakpointManager* GetBreakpointManager() override;
	ITraceLogger* GetTraceLogger() override;
	ISerializable* GetSerializableCpu() override;

	BaseState& GetState() override;
	void GetPpuState(BaseState& state) override;
	void SetPpuState(BaseState& state) override;
};
