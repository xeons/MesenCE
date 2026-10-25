#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "MD/MdTypes.h"

class DisassemblyInfo;
class Debugger;
class MdVdp;

class MdZ80TraceLogger : public BaseTraceLogger<MdZ80TraceLogger, MdZ80State>
{
private:
	MdVdp* _vdp = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	MdZ80TraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp);

	void GetTraceRow(string& output, MdZ80State& cpuState, TraceLogPpuState& vdpState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(MdZ80State& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(MdZ80State& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(MdZ80State& state) { return (uint8_t)state.SP; }
};
