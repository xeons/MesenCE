#include "pch.h"
#include "MD/Debugger/DummyMdZ80.h"
#include "MD/Debugger/MdZ80Debugger.h"
#include "MD/Debugger/MdZ80TraceLogger.h"
#include "MD/MdZ80.h"
#include "MD/MdVdp.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "SMS/Debugger/SmsAssembler.h"
#include "SMS/Debugger/SmsDisUtils.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/ExpressionEvaluator.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/BaseEventManager.h"
#include "Debugger/StepBackManager.h"
#include "Utilities/HexUtilities.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/MemoryOperationType.h"

MdZ80Debugger::MdZ80Debugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_emu = debugger->GetEmulator();

	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_console = ((MdConsole*)debugger->GetConsole());

	_cpu = _console->GetZ80();
	_vdp = _console->GetVdp();
	_memoryManager = _console->GetMemoryManager();

	_settings = debugger->GetEmulator()->GetSettings();

	//Events are shown in the 68000's event viewer
	_eventManager = debugger->GetEventManager(CpuType::Md);

	_traceLogger.reset(new MdZ80TraceLogger(debugger, this, _vdp));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::MdZ80, _eventManager));
	_step.reset(new StepRequest());
	_assembler.reset(new SmsAssembler(debugger->GetLabelManager()));

	_dummyCpu.reset(new DummyMdZ80());
	_dummyCpu->Init(_emu, _console, _memoryManager);
}

MdZ80Debugger::~MdZ80Debugger()
{
}

void MdZ80Debugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

void MdZ80Debugger::ProcessInstruction()
{
	MdZ80State& state = _cpu->GetState();
	uint16_t pc = state.PC;
	AddressInfo addressInfo = _memoryManager->GetZ80AbsoluteAddress(pc);
	uint8_t value = _memoryManager->DebugReadZ80(pc);
	MemoryOperationInfo operation(pc, value, MemoryOperationType::ExecOpCode, MemoryType::MdZ80Memory);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = state.CycleCount;

	if(addressInfo.Address >= 0) {
		_disassembler->BuildCache(addressInfo, 0, CpuType::MdZ80);
	}

	ProcessCallStackUpdates(addressInfo, pc, state.SP);

	if(value == 0xED) {
		_prevOpCode = 0xED | (_memoryManager->DebugReadZ80(pc + 1) << 8);
	} else {
		_prevOpCode = value;
	}
	_prevProgramCounter = pc;
	_prevStackPointer = state.SP;

	_step->ProcessCpuExec();

	if(_step->StepCount != 0 && _breakpointManager->HasBreakpoints() && _settings->GetDebugConfig().UsePredictiveBreakpoints) {
		_dummyCpu->SetDummyState(state);
		_dummyCpu->Exec();
		for(uint32_t i = 1; i < _dummyCpu->GetOperationCount(); i++) {
			MemoryOperationInfo memOp = _dummyCpu->GetOperationInfo(i);
			if(_breakpointManager->HasBreakpointForType(memOp.Type)) {
				AddressInfo absAddr = _memoryManager->GetZ80AbsoluteAddress(memOp.Address);
				_debugger->ProcessPredictiveBreakpoint(CpuType::MdZ80, _breakpointManager.get(), memOp, absAddr);
			}
		}
	}

	_debugger->ProcessBreakConditions(CpuType::MdZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void MdZ80Debugger::ProcessRegisterAccess(MemoryOperationInfo& operation)
{
	//YM2612, bank register, VDP and PSG accesses are shown in the event viewer
	if(operation.Address >= 0x4000 && operation.Address < 0x8000 && _eventManager) {
		_eventManager->AddEvent(DebugEventType::Register, operation);
	}
}

void MdZ80Debugger::ProcessRead(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = _memoryManager->GetZ80AbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::MdZ80Memory);
	InstructionProgress.LastMemOperation = operation;
	uint64_t masterClock = _console->GetMasterClock();

	if(type == MemoryOperationType::ExecOpCode) {
		if(_traceLogger->IsEnabled()) {
			DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, addr, 0, CpuType::MdZ80);
			_traceLogger->Log(_cpu->GetState(), disInfo, operation, addressInfo);
		}
		_memoryAccessCounter->ProcessMemoryExec(addressInfo, masterClock);
		if(_step->ProcessCpuCycle()) {
			_debugger->SleepUntilResume(CpuType::MdZ80, BreakSource::CpuStep, &operation);
		}
	} else if(type == MemoryOperationType::ExecOperand) {
		if(_traceLogger->IsEnabled()) {
			_traceLogger->LogNonExec(operation, addressInfo);
		}

		_memoryAccessCounter->ProcessMemoryExec(addressInfo, masterClock);
		_step->ProcessCpuCycle();
		_debugger->ProcessBreakConditions(CpuType::MdZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
	} else {
		if(_traceLogger->IsEnabled()) {
			_traceLogger->LogNonExec(operation, addressInfo);
		}

		ProcessRegisterAccess(operation);

		if(addr < 0x4000) {
			ReadResult result = _memoryAccessCounter->ProcessMemoryRead(addressInfo, masterClock);
			if(result != ReadResult::Normal) {
				//Memory access was a read on an uninitialized memory address
				if(result == ReadResult::FirstUninitRead) {
					//Only warn the first time
					_debugger->Log("[Z80] Uninitialized memory read: $" + HexUtilities::ToHex((uint16_t)addr));
				}
				if(_settings->CheckDebuggerFlag(DebuggerFlags::MdZ80DebuggerEnabled) && _settings->GetDebugConfig().BreakOnUninitRead) {
					_step->Break(BreakSource::BreakOnUninitMemoryRead);
				}
			}
		} else {
			_memoryAccessCounter->ProcessMemoryRead(addressInfo, masterClock);
		}

		_step->ProcessCpuCycle();
		_debugger->ProcessBreakConditions(CpuType::MdZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
	}
}

void MdZ80Debugger::ProcessWrite(uint32_t addr, uint8_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = _memoryManager->GetZ80AbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::MdZ80Memory);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Type == MemoryType::MdZ80Ram) {
		_disassembler->InvalidateCache(addressInfo, CpuType::MdZ80);
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	ProcessRegisterAccess(operation);

	_memoryAccessCounter->ProcessMemoryWrite(addressInfo, _console->GetMasterClock());
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions(CpuType::MdZ80, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void MdZ80Debugger::Run()
{
	_step.reset(new StepRequest());
}

void MdZ80Debugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);

	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;
		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;

		case StepType::StepOver:
			if(SmsDisUtils::IsJumpToSub(_prevOpCode)) {
				step.BreakAddress = _prevProgramCounter + GetPrevOpCodeSize();
				step.BreakStackPointer = _prevStackPointer;
			} else {
				//For any other instruction, step over is the same as step into
				step.StepCount = 1;
			}
			break;

		case StepType::CpuCycleStep: step.CpuCycleStepCount = stepCount; break;

		default:
			//PPU steps are handled by the 68000's debugger
			break;
	}

	_step.reset(new StepRequest(step));
}

uint8_t MdZ80Debugger::GetPrevOpCodeSize()
{
	return SmsDisUtils::GetOpSize(_prevOpCode, _prevProgramCounter, MemoryType::MdZ80Memory, _debugger->GetMemoryDumper());
}

void MdZ80Debugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint16_t destPc, uint16_t sp)
{
	if(SmsDisUtils::IsJumpToSub(_prevOpCode) && destPc != _prevProgramCounter + GetPrevOpCodeSize()) {
		//CALL and RST, and PC doesn't match the next instruction, so the call was (probably) done
		uint8_t opSize = GetPrevOpCodeSize();
		uint16_t returnPc = _prevProgramCounter + opSize;
		AddressInfo src = _memoryManager->GetZ80AbsoluteAddress(_prevProgramCounter);
		AddressInfo ret = _memoryManager->GetZ80AbsoluteAddress(returnPc);
		_callstackManager->Push(src, _prevProgramCounter, destAddr, destPc, ret, returnPc, _prevStackPointer, StackFrameFlags::None);
	} else if(SmsDisUtils::IsReturnInstruction(_prevOpCode)) {
		if(destPc != _prevProgramCounter + GetPrevOpCodeSize()) {
			//RET used, and PC doesn't match the next instruction, so the ret was (probably) taken
			_callstackManager->Pop(destAddr, destPc, sp);
		}

		if(_step->BreakAddress == (int32_t)destPc && _step->BreakStackPointer == sp) {
			//RET/RETI - if we're on the expected return address, break immediately (for step over/step out)
			_step->Break(BreakSource::CpuStep);
		}
	}
}

void MdZ80Debugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	AddressInfo ret = _memoryManager->GetZ80AbsoluteAddress(originalPc);
	AddressInfo dest = _memoryManager->GetZ80AbsoluteAddress(currentPc);

	uint16_t originalSp = _cpu->GetState().SP + 2;
	_prevStackPointer = originalSp;

	//If a call/return occurred just before IRQ, it needs to be processed now
	ProcessCallStackUpdates(ret, originalPc, originalSp);
	ResetPrevOpCode();

	_debugger->InternalProcessInterrupt(CpuType::MdZ80, *this, *_step.get(), ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi);
}

DebuggerFeatures MdZ80Debugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq = true;
	features.RunToNmi = false;
	features.StepOver = true;
	features.StepOut = true;
	features.StepBack = true;
	features.CallStack = true;
	features.CpuCycleStep = true;
	features.ChangeProgramCounter = AllowChangeProgramCounter;

	//The Z80's INT line is driven by the VDP's vertical interrupt (IM 1)
	features.CpuVectors[0] = { "IRQ", 0x38, VectorType::Direct };
	features.CpuVectorCount = 1;

	return features;
}

void MdZ80Debugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	if(!updateDebuggerOnly) {
		_cpu->GetState().PC = (uint16_t)addr;
	}
	_prevOpCode = _memoryManager->DebugReadZ80((uint16_t)addr);
	_prevProgramCounter = (uint16_t)addr;
	_prevStackPointer = _cpu->GetState().SP;
}

uint32_t MdZ80Debugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : _cpu->GetState().PC;
}

uint64_t MdZ80Debugger::GetCpuCycleCount(bool forProfiler)
{
	//The Z80's instruction cycle counter doesn't advance while it is stopped (bus request/reset), use the elapsed Z80 clocks instead
	return _cpu->GetState().MasterClock / MdConstants::Z80Divider;
}

void MdZ80Debugger::ResetPrevOpCode()
{
	_prevOpCode = 0;
}

BaseEventManager* MdZ80Debugger::GetEventManager()
{
	return _eventManager;
}

IAssembler* MdZ80Debugger::GetAssembler()
{
	return _assembler.get();
}

CallstackManager* MdZ80Debugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* MdZ80Debugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

BaseState& MdZ80Debugger::GetState()
{
	return _cpu->GetState();
}

void MdZ80Debugger::GetPpuState(BaseState& state)
{
	(MdVdpState&)state = _vdp->GetState();
}

void MdZ80Debugger::SetPpuState(BaseState& srcState)
{
	MdVdpState& dstState = _vdp->GetState();
	dstState = (MdVdpState&)srcState;
}

ITraceLogger* MdZ80Debugger::GetTraceLogger()
{
	return _traceLogger.get();
}

ISerializable* MdZ80Debugger::GetSerializableCpu()
{
	return _cpu;
}

bool MdZ80Debugger::SaveRomToDisk(string filename, bool saveAsIps, CdlStripOption stripOption)
{
	//The Z80 has no ROM of its own - the 68000's debugger handles saving the cartridge ROM
	return false;
}
