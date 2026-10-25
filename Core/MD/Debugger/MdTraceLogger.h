#pragma once
#include "pch.h"
#include "Debugger/BaseTraceLogger.h"
#include "MD/MdTypes.h"

class DisassemblyInfo;
class Debugger;
class MdVdp;

class MdTraceLogger : public BaseTraceLogger<MdTraceLogger, MdCpuState>
{
private:
	MdVdp* _vdp = nullptr;

protected:
	RowDataType GetFormatTagType(string& tag) override;

public:
	MdTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp);

	void GetTraceRow(string& output, MdCpuState& cpuState, TraceLogPpuState& vdpState, DisassemblyInfo& disassemblyInfo);
	void LogPpuState();

	__forceinline uint32_t GetProgramCounter(MdCpuState& state) { return state.PC; }
	__forceinline uint64_t GetCycleCount(MdCpuState& state) { return state.CycleCount; }
	__forceinline uint8_t GetStackPointer(MdCpuState& state) { return (uint8_t)state.A[7]; }
};
