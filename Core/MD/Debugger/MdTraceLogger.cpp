#include "pch.h"
#include "MD/Debugger/MdTraceLogger.h"
#include "MD/MdVdp.h"
#include "MD/MdTypes.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugTypes.h"
#include "Utilities/HexUtilities.h"

MdTraceLogger::MdTraceLogger(Debugger* debugger, IDebugger* cpuDebugger, MdVdp* vdp) : BaseTraceLogger(debugger, cpuDebugger, CpuType::Md)
{
	_vdp = vdp;
}

RowDataType MdTraceLogger::GetFormatTagType(string& tag)
{
	if(tag.size() == 2 && (tag[0] == 'D' || tag[0] == 'A') && tag[1] >= '0' && tag[1] <= '7') {
		//D0-D7 map to R0-R7, A0-A7 map to R8-R15
		return (RowDataType)((int)RowDataType::R0 + (tag[0] == 'A' ? 8 : 0) + (tag[1] - '0'));
	} else if(tag == "SR") {
		return RowDataType::SR;
	} else if(tag == "SP") {
		return RowDataType::SP;
	} else if(tag == "USP") {
		return RowDataType::USP;
	} else if(tag == "SSP") {
		return RowDataType::SSP;
	} else if(tag == "Flags") {
		return RowDataType::PS;
	} else {
		return RowDataType::Text;
	}
}

void MdTraceLogger::GetTraceRow(string& output, MdCpuState& cpuState, TraceLogPpuState& vdpState, DisassemblyInfo& disassemblyInfo)
{
	constexpr char activeStatusLetters[5] = { 'X', 'N', 'Z', 'V', 'C' };
	constexpr char inactiveStatusLetters[5] = { 'x', 'n', 'z', 'v', 'c' };

	bool supervisor = (cpuState.SR & MdCpuFlags::Supervisor) != 0;

	for(RowPart& rowPart : _rowParts) {
		switch(rowPart.DataType) {
			case RowDataType::R0: case RowDataType::R1: case RowDataType::R2: case RowDataType::R3:
			case RowDataType::R4: case RowDataType::R5: case RowDataType::R6: case RowDataType::R7:
				WriteIntValue(output, cpuState.D[(int)rowPart.DataType - (int)RowDataType::R0], rowPart);
				break;

			case RowDataType::R8: case RowDataType::R9: case RowDataType::R10: case RowDataType::R11:
			case RowDataType::R12: case RowDataType::R13: case RowDataType::R14: case RowDataType::R15:
				WriteIntValue(output, cpuState.A[(int)rowPart.DataType - (int)RowDataType::R8], rowPart);
				break;

			case RowDataType::SR: WriteIntValue(output, cpuState.SR, rowPart); break;
			case RowDataType::SP: WriteIntValue(output, cpuState.A[7], rowPart); break;
			case RowDataType::USP: WriteIntValue(output, supervisor ? cpuState.USP : cpuState.A[7], rowPart); break;
			case RowDataType::SSP: WriteIntValue(output, supervisor ? cpuState.A[7] : cpuState.SSP, rowPart); break;
			case RowDataType::PS: GetStatusFlag(activeStatusLetters, inactiveStatusLetters, output, cpuState.SR & 0x1F, rowPart, 5); break;
			default: ProcessSharedTag(rowPart, output, cpuState, vdpState, disassemblyInfo); break;
		}
	}
}

void MdTraceLogger::LogPpuState()
{
	_ppuState[_currentPos] = {
		_vdp->GetHClock(),
		_vdp->GetHClock(),
		_vdp->GetScanline(),
		_vdp->GetFrameCount()
	};
}
