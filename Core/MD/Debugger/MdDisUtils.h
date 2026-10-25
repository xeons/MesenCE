#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class MdConsole;
class LabelManager;
class EmuSettings;
class MemoryDumper;
class DisassemblyInfo;
struct MdCpuState;

class MdDisUtils
{
public:
	static constexpr uint8_t MaxOpSize = 10;

	static void GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings);
	static EffectiveAddressInfo GetEffectiveAddress(DisassemblyInfo& info, MdConsole* console, MdCpuState& state);

	static uint8_t GetOpSize(uint16_t opCode);
	static uint8_t GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper);
	static uint16_t GetOpCode(DisassemblyInfo& info);

	static bool IsJumpToSub(uint16_t opCode);
	static bool IsReturnInstruction(uint16_t opCode);
	static bool IsUnconditionalJump(uint16_t opCode);
	static bool IsConditionalJump(uint16_t opCode);
	static CdlFlags::CdlFlags GetOpFlags(uint16_t opCode, uint32_t pc, uint32_t prevPc, uint8_t opSize);
};
