#include "pch.h"
#include "MD/Debugger/DummyMdCpu.h"
#include "MD/Debugger/MdDebugger.h"
#include "MD/Debugger/MdAssembler.h"
#include "MD/Debugger/MdDisUtils.h"
#include "MD/Debugger/MdEventManager.h"
#include "MD/Debugger/MdTraceLogger.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/Input/MdController.h"
#include "MD/MdCpu.h"
#include "MD/MdVdp.h"
#include "MD/MdConsole.h"
#include "MD/MdMemoryManager.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Disassembler.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/CallstackManager.h"
#include "Debugger/BreakpointManager.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/CodeDataLogger.h"
#include "Debugger/BaseEventManager.h"
#include "Debugger/StepBackManager.h"
#include "Utilities/Patches/IpsPatcher.h"
#include "Utilities/HexUtilities.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"
#include "Shared/BaseControlManager.h"
#include "Shared/MemoryOperationType.h"

MdDebugger::MdDebugger(Debugger* debugger) : IDebugger(debugger->GetEmulator())
{
	_debugger = debugger;
	_memoryDumper = debugger->GetMemoryDumper();
	_emu = debugger->GetEmulator();

	_disassembler = debugger->GetDisassembler();
	_memoryAccessCounter = debugger->GetMemoryAccessCounter();

	_console = (MdConsole*)debugger->GetConsole();

	_cpu = _console->GetCpu();
	_vdp = _console->GetVdp();
	_memoryManager = _console->GetMemoryManager();

	_settings = debugger->GetEmulator()->GetSettings();

	_codeDataLogger.reset(new CodeDataLogger(debugger, MemoryType::MdPrgRom, _emu->GetMemory(MemoryType::MdPrgRom).Size, CpuType::Md, _emu->GetCrc32()));
	_cdlFile = _codeDataLogger->GetCdlFilePath(_emu->GetRomInfo().RomFile.GetFileName());
	_codeDataLogger->LoadCdlFile(_cdlFile, _settings->GetDebugConfig().AutoResetCdl);

	_traceLogger.reset(new MdTraceLogger(debugger, this, _vdp));
	_ppuTools.reset(new MdVdpTools(debugger, debugger->GetEmulator(), _console));

	_stepBackManager.reset(new StepBackManager(_emu, this));
	_eventManager.reset(new MdEventManager(debugger, _console));
	_callstackManager.reset(new CallstackManager(debugger, this));
	_breakpointManager.reset(new BreakpointManager(debugger, this, CpuType::Md, _eventManager.get()));
	_step.reset(new StepRequest());
	_assembler.reset(new MdAssembler(debugger->GetLabelManager()));

	_dummyCpu.reset(new DummyMdCpu());
	_dummyCpu->Init(_emu, _console, _memoryManager);

	ResetPrevOpCode();
}

MdDebugger::~MdDebugger()
{
	_codeDataLogger->SaveCdlFile(_cdlFile);
}

void MdDebugger::Reset()
{
	_callstackManager->Clear();
	ResetPrevOpCode();
}

void MdDebugger::ProcessInstruction()
{
	MdCpuState& state = _cpu->GetState();
	uint32_t pc = state.PC & 0xFFFFFF;
	uint16_t opCode = state.IR;
	AddressInfo addressInfo = _memoryManager->GetAbsoluteAddress(pc);
	MemoryOperationInfo operation(pc, opCode, MemoryOperationType::ExecOpCode, MemoryType::MdMemory);
	InstructionProgress.LastMemOperation = operation;
	InstructionProgress.StartCycle = state.CycleCount;

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::MdPrgRom) {
			uint8_t opSize = MdDisUtils::GetOpSize(opCode);
			_codeDataLogger->SetCode<2>(addressInfo.Address, MdDisUtils::GetOpFlags(_prevOpCode, pc, _prevProgramCounter, GetPrevOpCodeSize()));
			for(int i = 2; i < opSize; i += 2) {
				_codeDataLogger->SetCode<0, 2>(addressInfo.Address + i);
			}
		}
		_disassembler->BuildCache(addressInfo, 0, CpuType::Md);
	}

	ProcessCallStackUpdates(addressInfo, pc, state.A[7]);

	if(_step->BreakAddress == (int32_t)pc && _step->BreakStackPointer == state.A[7] && MdDisUtils::IsReturnInstruction(_prevOpCode)) {
		//If we're on the expected return address, break immediately (for step over/step out)
		_step->Break(BreakSource::CpuStep);
	}

	_prevOpCode = opCode;
	_prevProgramCounter = pc;
	_prevStackPointer = state.A[7];

	_step->ProcessCpuExec();

	if(_step->StepCount != 0 && _breakpointManager->HasBreakpoints() && _settings->GetDebugConfig().UsePredictiveBreakpoints) {
		_dummyCpu->SetDummyState(state);
		_dummyCpu->Exec();
		for(uint32_t i = 0; i < _dummyCpu->GetOperationCount(); i++) {
			MemoryOperationInfo memOp = _dummyCpu->GetOperationInfo(i);
			if(memOp.Type == MemoryOperationType::ExecOpCode || memOp.Type == MemoryOperationType::ExecOperand) {
				//Prefetch reads are not instruction executions
				continue;
			}

			if(_breakpointManager->HasBreakpointForType(memOp.Type)) {
				AddressInfo absAddr = _memoryManager->GetAbsoluteAddress(memOp.Address);
				_debugger->ProcessPredictiveBreakpoint(CpuType::Md, _breakpointManager.get(), memOp, absAddr);
			}
		}
	}

	_debugger->ProcessBreakConditions(CpuType::Md, *_step.get(), _breakpointManager.get(), operation, addressInfo);

	if(_traceLogger->IsEnabled()) {
		DisassemblyInfo disInfo = _disassembler->GetDisassemblyInfo(addressInfo, pc, 0, CpuType::Md);
		_traceLogger->Log(state, disInfo, operation, addressInfo);
	}
}

template<uint8_t accessWidth>
void MdDebugger::ProcessRead(uint32_t addr, uint16_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = _memoryManager->GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::MdMemory);

	if(type == MemoryOperationType::ExecOpCode || type == MemoryOperationType::ExecOperand) {
		//Prefetch queue reads - code/data logging and breakpoints are handled in ProcessInstruction
		_memoryAccessCounter->ProcessMemoryExec<accessWidth>(addressInfo, _console->GetMasterClock());
		return;
	}

	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0) {
		if(addressInfo.Type == MemoryType::MdPrgRom) {
			_codeDataLogger->SetData<0, accessWidth>(addressInfo.Address);
		}

		ReadResult result = _memoryAccessCounter->ProcessMemoryRead<accessWidth>(addressInfo, _console->GetMasterClock());
		if(result != ReadResult::Normal) {
			//Memory access was a read on an uninitialized memory address
			if((int)result & (int)ReadResult::FirstUninitRead) {
				//Only warn the first time
				_debugger->Log("[MD] Uninitialized memory read: $" + HexUtilities::ToHex24(addr));
			}
			if(_settings->CheckDebuggerFlag(DebuggerFlags::MdDebuggerEnabled) && _settings->GetDebugConfig().BreakOnUninitRead) {
				_step->Break(BreakSource::BreakOnUninitMemoryRead);
			}
		}
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	if((addr >= 0xA00000 && addr < 0xA20000) || (addr >= 0xC00000 && addr < 0xE00000)) {
		_eventManager->AddEvent(DebugEventType::Register, operation);
	}

	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions<accessWidth>(CpuType::Md, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

template<uint8_t accessWidth>
void MdDebugger::ProcessWrite(uint32_t addr, uint16_t value, MemoryOperationType type)
{
	AddressInfo addressInfo = _memoryManager->GetAbsoluteAddress(addr);
	MemoryOperationInfo operation(addr, value, type, MemoryType::MdMemory);
	InstructionProgress.LastMemOperation = operation;

	if(addressInfo.Address >= 0) {
		switch(addressInfo.Type) {
			case MemoryType::MdWorkRam:
			case MemoryType::MdSaveRam: {
				//Invalidate using the address of the last byte written, to cover the instructions that overlap both bytes
				AddressInfo lastByte = { addressInfo.Address + accessWidth - 1, addressInfo.Type };
				_disassembler->InvalidateCache(lastByte, CpuType::Md);
				break;
			}

			case MemoryType::MdZ80Ram:
				//The 68000 uploads the Z80's program
				_disassembler->InvalidateCache(addressInfo, CpuType::MdZ80);
				break;

			default:
				break;
		}
	}

	if(_traceLogger->IsEnabled()) {
		_traceLogger->LogNonExec(operation, addressInfo);
	}

	if((addr >= 0xA00000 && addr < 0xA20000) || (addr >= 0xC00000 && addr < 0xE00000)) {
		_eventManager->AddEvent(DebugEventType::Register, operation);
	}

	_memoryAccessCounter->ProcessMemoryWrite<accessWidth>(addressInfo, _console->GetMasterClock());
	_step->ProcessCpuCycle();
	_debugger->ProcessBreakConditions<accessWidth>(CpuType::Md, *_step.get(), _breakpointManager.get(), operation, addressInfo);
}

void MdDebugger::Run()
{
	_step.reset(new StepRequest());
	_breakFrameCount = 0;
}

void MdDebugger::Step(int32_t stepCount, StepType type)
{
	StepRequest step(type);
	_breakFrameCount = 0;

	switch(type) {
		case StepType::Step: step.StepCount = stepCount; break;

		case StepType::StepOut:
			step.BreakAddress = _callstackManager->GetReturnAddress();
			step.BreakStackPointer = _callstackManager->GetReturnStackPointer();
			break;

		case StepType::StepOver:
			if(MdDisUtils::IsJumpToSub(_prevOpCode)) {
				step.BreakAddress = _prevProgramCounter + GetPrevOpCodeSize();
				step.BreakStackPointer = _prevStackPointer;
			} else {
				//For any other instruction, step over is the same as step into
				step.StepCount = 1;
			}
			break;

		case StepType::CpuCycleStep: step.CpuCycleStepCount = stepCount; break;
		case StepType::PpuStep: step.PpuStepCount = stepCount; break;
		case StepType::PpuScanline: step.BreakScanline = (_vdp->GetScanline() + stepCount) % _vdp->GetScanlineCount(); break;
		case StepType::PpuFrame:
			step.BreakScanline = _vdp->GetScanline();
			_breakFrameCount = _vdp->GetFrameCount() + std::max(stepCount, 1);
			break;
		case StepType::SpecificScanline: step.BreakScanline = stepCount; break;
	}

	_step.reset(new StepRequest(step));
}

StepBackConfig MdDebugger::GetStepBackConfig()
{
	uint32_t cyclesPerScanline = MdConstants::ClocksPerScanline / MdConstants::CpuDivider;
	return {
		GetCpuCycleCount(false),
		cyclesPerScanline,
		cyclesPerScanline * _vdp->GetScanlineCount()
	};
}

void MdDebugger::DrawPartialFrame()
{
	_vdp->DebugSendFrame();
}

uint8_t MdDebugger::GetPrevOpCodeSize()
{
	return MdDisUtils::GetOpSize(_prevOpCode);
}

void MdDebugger::ProcessCallStackUpdates(AddressInfo& destAddr, uint32_t destPc, uint32_t sp)
{
	if(MdDisUtils::IsJumpToSub(_prevOpCode) && destPc != _prevProgramCounter + GetPrevOpCodeSize()) {
		//JSR/BSR/TRAP, and PC doesn't match the next instruction, so the call was (probably) done
		uint32_t returnPc = (_prevProgramCounter + GetPrevOpCodeSize()) & 0xFFFFFF;
		AddressInfo src = _memoryManager->GetAbsoluteAddress(_prevProgramCounter);
		AddressInfo ret = _memoryManager->GetAbsoluteAddress(returnPc);
		_callstackManager->Push(src, _prevProgramCounter, destAddr, destPc, ret, returnPc, _prevStackPointer, StackFrameFlags::None);
	} else if(MdDisUtils::IsReturnInstruction(_prevOpCode) && destPc != _prevProgramCounter + GetPrevOpCodeSize()) {
		//RTS/RTE/RTR used, and PC doesn't match the next instruction, so the return was (probably) taken
		_callstackManager->Pop(destAddr, destPc, sp);
	}
}

void MdDebugger::ProcessInterrupt(uint32_t originalPc, uint32_t currentPc, bool forNmi)
{
	originalPc &= 0xFFFFFF;
	currentPc &= 0xFFFFFF;

	AddressInfo ret = _memoryManager->GetAbsoluteAddress(originalPc);
	AddressInfo dest = _memoryManager->GetAbsoluteAddress(currentPc);

	if(dest.Type == MemoryType::MdPrgRom && dest.Address >= 0) {
		_codeDataLogger->SetCode(dest.Address, CdlFlags::SubEntryPoint);
	}

	//The exception stack frame is 6 bytes (SR + PC)
	uint32_t originalSp = _cpu->GetState().A[7] + 6;

	//If a call/return occurred just before the interrupt, it needs to be processed now
	ProcessCallStackUpdates(ret, originalPc, originalSp);
	ResetPrevOpCode();

	_debugger->InternalProcessInterrupt(CpuType::Md, *this, *_step.get(), ret, originalPc, dest, currentPc, ret, originalPc, originalSp, forNmi);
}

void MdDebugger::ProcessPpuCycle()
{
	if(_ppuTools->HasOpenedViewer()) {
		_ppuTools->UpdateViewers(_vdp->GetScanline(), _vdp->GetHClock());
	}

	//The VDP calls this once per access slot - scanline breaks happen on the first slot of the requested scanline
	uint16_t scanline = _vdp->GetScanline();
	bool isLineStart = scanline != _prevPpuScanline;
	_prevPpuScanline = scanline;

	if(_step->HasRequest) {
		if(_step->HasScanlineBreakRequest()) {
			if(isLineStart && scanline == _step->BreakScanline && _vdp->GetFrameCount() >= _breakFrameCount) {
				_debugger->SleepUntilResume(CpuType::Md, _step->GetBreakSource());
			}
		} else if(_step->PpuStepCount > 0) {
			_step->PpuStepCount--;
			if(_step->PpuStepCount == 0) {
				_debugger->SleepUntilResume(CpuType::Md, _step->GetBreakSource());
			}
		}
	}
}

DebuggerFeatures MdDebugger::GetSupportedFeatures()
{
	DebuggerFeatures features = {};
	features.RunToIrq = true;
	features.RunToNmi = false;
	features.StepOver = true;
	features.StepOut = true;
	features.StepBack = true;
	features.CallStack = true;
	features.CpuCycleStep = false;
	features.ChangeProgramCounter = AllowChangeProgramCounter;

	features.CpuVectors[0] = { "Reset", 0x04, VectorType::IndirectBigEndian32 };
	features.CpuVectors[1] = { "Bus Error", 0x08, VectorType::IndirectBigEndian32 };
	features.CpuVectors[2] = { "Address Error", 0x0C, VectorType::IndirectBigEndian32 };
	features.CpuVectors[3] = { "Illegal", 0x10, VectorType::IndirectBigEndian32 };
	features.CpuVectors[4] = { "Divide by 0", 0x14, VectorType::IndirectBigEndian32 };
	features.CpuVectors[5] = { "CHK", 0x18, VectorType::IndirectBigEndian32 };
	features.CpuVectors[6] = { "TRAPV", 0x1C, VectorType::IndirectBigEndian32 };
	features.CpuVectors[7] = { "Privilege", 0x20, VectorType::IndirectBigEndian32 };
	features.CpuVectors[8] = { "Trace", 0x24, VectorType::IndirectBigEndian32 };
	features.CpuVectors[9] = { "Line A", 0x28, VectorType::IndirectBigEndian32 };
	features.CpuVectors[10] = { "Line F", 0x2C, VectorType::IndirectBigEndian32 };
	features.CpuVectors[11] = { "Spurious IRQ", 0x60, VectorType::IndirectBigEndian32 };
	features.CpuVectors[12] = { "EXT (Level 2)", 0x68, VectorType::IndirectBigEndian32 };
	features.CpuVectors[13] = { "HINT (Level 4)", 0x70, VectorType::IndirectBigEndian32 };
	features.CpuVectors[14] = { "VINT (Level 6)", 0x78, VectorType::IndirectBigEndian32 };
	features.CpuVectors[15] = { "TRAP #0", 0x80, VectorType::IndirectBigEndian32 };
	features.CpuVectorCount = 16;

	return features;
}

void MdDebugger::SetProgramCounter(uint32_t addr, bool updateDebuggerOnly)
{
	addr &= 0xFFFFFE;
	MdCpuState& state = _cpu->GetState();
	if(!updateDebuggerOnly) {
		//Reload the prefetch queue from the new address
		state.PC = addr;
		state.IR = _memoryManager->DebugReadWord(addr);
		state.IRC = _memoryManager->DebugReadWord(addr + 2);
	}

	_prevOpCode = state.IR;
	_prevProgramCounter = addr;
	_prevStackPointer = state.A[7];
}

uint32_t MdDebugger::GetProgramCounter(bool getInstPc)
{
	return getInstPc ? _prevProgramCounter : (_cpu->GetState().PC & 0xFFFFFF);
}

uint64_t MdDebugger::GetCpuCycleCount(bool forProfiler)
{
	return _cpu->GetCycleCount();
}

void MdDebugger::ResetPrevOpCode()
{
	_prevOpCode = 0x4E71; //NOP
}

BaseEventManager* MdDebugger::GetEventManager()
{
	return _eventManager.get();
}

IAssembler* MdDebugger::GetAssembler()
{
	return _assembler.get();
}

CallstackManager* MdDebugger::GetCallstackManager()
{
	return _callstackManager.get();
}

BreakpointManager* MdDebugger::GetBreakpointManager()
{
	return _breakpointManager.get();
}

BaseState& MdDebugger::GetState()
{
	return _cpu->GetState();
}

void MdDebugger::GetPpuState(BaseState& state)
{
	(MdVdpState&)state = _vdp->GetState();
}

void MdDebugger::SetPpuState(BaseState& srcState)
{
	MdVdpState& dstState = _vdp->GetState();
	dstState = (MdVdpState&)srcState;
}

ITraceLogger* MdDebugger::GetTraceLogger()
{
	return _traceLogger.get();
}

PpuTools* MdDebugger::GetPpuTools()
{
	return _ppuTools.get();
}

ISerializable* MdDebugger::GetSerializableCpu()
{
	return _cpu;
}

bool MdDebugger::SaveRomToDisk(string filename, bool saveAsIps, CdlStripOption stripOption)
{
	vector<uint8_t> output;

	uint8_t* prgRom = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::MdPrgRom);
	uint32_t prgRomSize = _debugger->GetMemoryDumper()->GetMemorySize(MemoryType::MdPrgRom);
	vector<uint8_t> rom = vector<uint8_t>(prgRom, prgRom + prgRomSize);

	if(saveAsIps) {
		vector<uint8_t> originalRom;
		_emu->GetRomInfo().RomFile.ReadFile(originalRom);

		output = IpsPatcher::CreatePatch(originalRom, rom);
	} else {
		if(stripOption != CdlStripOption::StripNone) {
			_codeDataLogger->StripData(rom.data(), stripOption);

			//Preserve the vector table and the cartridge header
			memcpy(rom.data(), prgRom, std::min<uint32_t>(0x200, prgRomSize));
		}
		output = rom;
	}

	ofstream file(filename, ios::out | ios::binary);
	if(file) {
		file.write((char*)output.data(), output.size());
		file.close();
		return true;
	}
	return false;
}

void MdDebugger::ProcessInputOverrides(DebugControllerState inputOverrides[8])
{
	BaseControlManager* controlManager = _console->GetControlManager();
	for(int i = 0; i < 8; i++) {
		shared_ptr<MdController> controller = std::dynamic_pointer_cast<MdController>(controlManager->GetControlDeviceByIndex(i));
		if(controller && inputOverrides[i].HasPressedButton()) {
			controller->SetBitValue(MdController::Buttons::A, inputOverrides[i].A);
			controller->SetBitValue(MdController::Buttons::B, inputOverrides[i].B);
			controller->SetBitValue(MdController::Buttons::C, inputOverrides[i].X);
			controller->SetBitValue(MdController::Buttons::X, inputOverrides[i].Y);
			controller->SetBitValue(MdController::Buttons::Y, inputOverrides[i].L);
			controller->SetBitValue(MdController::Buttons::Z, inputOverrides[i].R);
			controller->SetBitValue(MdController::Buttons::Start, inputOverrides[i].Start);
			controller->SetBitValue(MdController::Buttons::Mode, inputOverrides[i].Select);
			controller->SetBitValue(MdController::Buttons::Up, inputOverrides[i].Up);
			controller->SetBitValue(MdController::Buttons::Down, inputOverrides[i].Down);
			controller->SetBitValue(MdController::Buttons::Left, inputOverrides[i].Left);
			controller->SetBitValue(MdController::Buttons::Right, inputOverrides[i].Right);
		}
	}
	controlManager->RefreshHubState();
}

template void MdDebugger::ProcessRead<1>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void MdDebugger::ProcessRead<2>(uint32_t addr, uint16_t value, MemoryOperationType type);

template void MdDebugger::ProcessWrite<1>(uint32_t addr, uint16_t value, MemoryOperationType type);
template void MdDebugger::ProcessWrite<2>(uint32_t addr, uint16_t value, MemoryOperationType type);
