#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"

class MdConsole;
class DisassemblyInfo;
struct MdZ80State;

//The Z80's disassembly is shared with the SMS (SmsDisUtils), except for effective address calculations
class MdZ80DisUtils
{
public:
	static EffectiveAddressInfo GetEffectiveAddress(DisassemblyInfo& info, MdConsole* console, MdZ80State& state);
};
