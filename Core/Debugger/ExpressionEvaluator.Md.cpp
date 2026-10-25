#include "pch.h"
#include "Debugger/ExpressionEvaluator.h"
#include "MD/Debugger/MdDebugger.h"
#include "MD/Debugger/MdZ80Debugger.h"
#include "MD/MdTypes.h"

unordered_map<string, int64_t>& ExpressionEvaluator::GetMdTokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "d0", EvalValues::R0 },
		{ "d1", EvalValues::R1 },
		{ "d2", EvalValues::R2 },
		{ "d3", EvalValues::R3 },
		{ "d4", EvalValues::R4 },
		{ "d5", EvalValues::R5 },
		{ "d6", EvalValues::R6 },
		{ "d7", EvalValues::R7 },
		{ "a0", EvalValues::R8 },
		{ "a1", EvalValues::R9 },
		{ "a2", EvalValues::R10 },
		{ "a3", EvalValues::R11 },
		{ "a4", EvalValues::R12 },
		{ "a5", EvalValues::R13 },
		{ "a6", EvalValues::R14 },
		{ "a7", EvalValues::R15 },
		{ "sp", EvalValues::RegSP },
		{ "usp", EvalValues::RegUSP },
		{ "ssp", EvalValues::RegSSP },
		{ "pc", EvalValues::RegPC },
		{ "sr", EvalValues::RegSR },

		{ "carry", EvalValues::RegPS_Carry },
		{ "overflow", EvalValues::RegPS_Overflow },
		{ "zero", EvalValues::RegPS_Zero },
		{ "negative", EvalValues::RegPS_Negative },
		{ "extend", EvalValues::RegPS_Extend },

		{ "frame", EvalValues::PpuFrameCount },
		{ "cycle", EvalValues::PpuCycle },
		{ "scanline", EvalValues::PpuScanline },
		{ "vblank", EvalValues::VerticalBlank },
		{ "spritecollision", EvalValues::SpriteCollision },
		{ "spriteoverflow", EvalValues::SpriteOverflow },
	};

	return supportedTokens;
}

int64_t ExpressionEvaluator::GetMdTokenValue(int64_t token, EvalResultType& resultType)
{
	auto ppu = [this]() -> MdVdpState {
		MdVdpState ppu;
		((MdDebugger*)_cpuDebugger)->GetPpuState(ppu);
		return ppu;
	};

	MdCpuState& s = (MdCpuState&)((MdDebugger*)_cpuDebugger)->GetState();
	bool supervisor = (s.SR & MdCpuFlags::Supervisor) != 0;
	switch(token) {
		case EvalValues::R0: case EvalValues::R1: case EvalValues::R2: case EvalValues::R3:
		case EvalValues::R4: case EvalValues::R5: case EvalValues::R6: case EvalValues::R7:
			return s.D[token - EvalValues::R0];

		case EvalValues::R8: case EvalValues::R9: case EvalValues::R10: case EvalValues::R11:
		case EvalValues::R12: case EvalValues::R13: case EvalValues::R14: case EvalValues::R15:
			return s.A[token - EvalValues::R8];

		case EvalValues::RegSP: return s.A[7];
		case EvalValues::RegUSP: return supervisor ? s.USP : s.A[7];
		case EvalValues::RegSSP: return supervisor ? s.A[7] : s.SSP;
		case EvalValues::RegPC: return s.PC & 0xFFFFFF;
		case EvalValues::RegSR: return s.SR;

		case EvalValues::RegPS_Carry: return ReturnBool(s.SR & MdCpuFlags::Carry, resultType);
		case EvalValues::RegPS_Overflow: return ReturnBool(s.SR & MdCpuFlags::Overflow, resultType);
		case EvalValues::RegPS_Zero: return ReturnBool(s.SR & MdCpuFlags::Zero, resultType);
		case EvalValues::RegPS_Negative: return ReturnBool(s.SR & MdCpuFlags::Negative, resultType);
		case EvalValues::RegPS_Extend: return ReturnBool(s.SR & MdCpuFlags::Extend, resultType);

		case EvalValues::PpuFrameCount: return ppu().FrameCount;
		case EvalValues::PpuCycle: return ppu().HClock;
		case EvalValues::PpuScanline: return ppu().Scanline;
		case EvalValues::VerticalBlank: return ReturnBool(ppu().InVerticalBlank, resultType);
		case EvalValues::SpriteCollision: return ReturnBool(ppu().SpriteCollision, resultType);
		case EvalValues::SpriteOverflow: return ReturnBool(ppu().SpriteOverflow, resultType);

		default: return 0;
	}
}

unordered_map<string, int64_t>& ExpressionEvaluator::GetMdZ80Tokens()
{
	static unordered_map<string, int64_t> supportedTokens = {
		{ "a", EvalValues::RegA },
		{ "b", EvalValues::RegB },
		{ "c", EvalValues::RegC },
		{ "d", EvalValues::RegD },
		{ "e", EvalValues::RegE },
		{ "f", EvalValues::RegF },
		{ "h", EvalValues::RegH },
		{ "l", EvalValues::RegL },
		{ "af", EvalValues::RegAF },
		{ "bc", EvalValues::RegBC },
		{ "de", EvalValues::RegDE },
		{ "hl", EvalValues::RegHL },
		{ "a'", EvalValues::RegAltA },
		{ "b'", EvalValues::RegAltB },
		{ "c'", EvalValues::RegAltC },
		{ "d'", EvalValues::RegAltD },
		{ "e'", EvalValues::RegAltE },
		{ "f'", EvalValues::RegAltF },
		{ "h'", EvalValues::RegAltH },
		{ "l'", EvalValues::RegAltL },
		{ "af'", EvalValues::RegAltAF },
		{ "bc'", EvalValues::RegAltBC },
		{ "de'", EvalValues::RegAltDE },
		{ "hl'", EvalValues::RegAltHL },
		{ "i", EvalValues::RegI },
		{ "r", EvalValues::RegR },
		{ "ix", EvalValues::RegIX },
		{ "iy", EvalValues::RegIY },
		{ "sp", EvalValues::RegSP },
		{ "pc", EvalValues::RegPC },

		{ "frame", EvalValues::PpuFrameCount },
		{ "cycle", EvalValues::PpuCycle },
		{ "scanline", EvalValues::PpuScanline },
	};

	return supportedTokens;
}

int64_t ExpressionEvaluator::GetMdZ80TokenValue(int64_t token, EvalResultType& resultType)
{
	auto ppu = [this]() -> MdVdpState {
		MdVdpState ppu;
		((MdZ80Debugger*)_cpuDebugger)->GetPpuState(ppu);
		return ppu;
	};

	MdZ80State& s = (MdZ80State&)((MdZ80Debugger*)_cpuDebugger)->GetState();
	switch(token) {
		case EvalValues::RegA: return s.A;
		case EvalValues::RegB: return s.B;
		case EvalValues::RegC: return s.C;
		case EvalValues::RegD: return s.D;
		case EvalValues::RegE: return s.E;
		case EvalValues::RegF: return s.Flags;
		case EvalValues::RegH: return s.H;
		case EvalValues::RegL: return s.L;
		case EvalValues::RegAF: return (s.A << 8) | s.Flags;
		case EvalValues::RegBC: return (s.B << 8) | s.C;
		case EvalValues::RegDE: return (s.D << 8) | s.E;
		case EvalValues::RegHL: return (s.H << 8) | s.L;
		case EvalValues::RegAltA: return s.AltA;
		case EvalValues::RegAltB: return s.AltB;
		case EvalValues::RegAltC: return s.AltC;
		case EvalValues::RegAltD: return s.AltD;
		case EvalValues::RegAltE: return s.AltE;
		case EvalValues::RegAltF: return s.AltFlags;
		case EvalValues::RegAltH: return s.AltH;
		case EvalValues::RegAltL: return s.AltL;
		case EvalValues::RegAltAF: return (s.AltA << 8) | s.AltFlags;
		case EvalValues::RegAltBC: return (s.AltB << 8) | s.AltC;
		case EvalValues::RegAltDE: return (s.AltD << 8) | s.AltE;
		case EvalValues::RegAltHL: return (s.AltH << 8) | s.AltL;
		case EvalValues::RegIX: return (s.IXH << 8) | s.IXL;
		case EvalValues::RegIY: return (s.IYH << 8) | s.IYL;
		case EvalValues::RegI: return s.I;
		case EvalValues::RegR: return s.R;
		case EvalValues::RegSP: return s.SP;
		case EvalValues::RegPC: return s.PC;

		case EvalValues::PpuFrameCount: return ppu().FrameCount;
		case EvalValues::PpuCycle: return ppu().HClock;
		case EvalValues::PpuScanline: return ppu().Scanline;

		default: return 0;
	}
}
