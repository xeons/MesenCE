#include "pch.h"
#include "MD/MdCpu.h"
#include "MD/MdTypes.h"
#include "MD/MdMemoryManager.h"
#include "Shared/Emulator.h"

namespace MdEaMode
{
	enum MdEaMode : uint16_t
	{
		DataReg = 0x001,
		AddrReg = 0x002,
		Indirect = 0x004,
		PostInc = 0x008,
		PreDec = 0x010,
		Disp = 0x020,
		Index = 0x040,
		AbsWord = 0x080,
		AbsLong = 0x100,
		PcDisp = 0x200,
		PcIndex = 0x400,
		Immediate = 0x800,

		All = 0xFFF,
		Data = 0xFFD,
		Memory = 0xFFC,
		Alterable = 0x1FF,
		DataAlterable = 0x1FD,
		MemoryAlterable = 0x1FC,
		Control = 0x7E4,
		ControlAlterable = 0x1E4,
	};
}

namespace MdArithOp
{
	enum MdArithOp
	{
		Add = 0,
		Sub = 1,
		And = 2,
		Or = 3,
		Eor = 4,
		Cmp = 5,
	};
}

namespace MdUnaryOp
{
	enum MdUnaryOp
	{
		Negx = 0,
		Clr = 1,
		Neg = 2,
		Not = 3,
	};
}

namespace MdShiftType
{
	enum MdShiftType
	{
		As = 0,
		Ls = 1,
		Rox = 2,
		Ro = 3,
	};
}

template<int size>
uint32_t MdCpu::Read(uint32_t addr)
{
	if constexpr(size == 1) {
		return ReadByte(addr);
	} else if constexpr(size == 2) {
		return ReadWord(addr);
	} else {
		return ReadLong(addr);
	}
}

template<int size>
void MdCpu::Write(uint32_t addr, uint32_t value)
{
	if constexpr(size == 1) {
		WriteByte(addr, (uint8_t)value);
	} else if constexpr(size == 2) {
		WriteWord(addr, (uint16_t)value);
	} else {
		WriteLong(addr, value);
	}
}

template<int size>
void MdCpu::WriteReversed(uint32_t addr, uint32_t value)
{
	if constexpr(size == 4) {
		WriteLongReversed(addr, value);
	} else {
		Write<size>(addr, value);
	}
}

template<int size, bool isMoveDst>
uint32_t MdCpu::GetEaAddress(uint8_t mode, uint8_t reg)
{
	switch(mode) {
		case 2: return _state.A[reg];

		case 3: {
			uint32_t addr = _state.A[reg];
			_state.A[reg] += (size == 1 && reg == 7) ? 2 : size;
			return addr;
		}

		case 4:
			if constexpr(!isMoveDst) {
				Idle(2);
			}
			_state.A[reg] -= (size == 1 && reg == 7) ? 2 : size;
			return _state.A[reg];

		case 5: return _state.A[reg] + (int16_t)ReadExt();

		case 6:
			Idle(2);
			return GetIndexedAddress(_state.A[reg]);

		default:
			switch(reg) {
				case 0: return (uint32_t)(int16_t)ReadExt();

				case 1: {
					uint32_t hi = ReadExt();
					return (hi << 16) | ReadExt();
				}

				case 2: {
					uint32_t base = _state.PC + 2;
					return base + (int16_t)ReadExt();
				}

				default: {
					Idle(2);
					uint32_t base = _state.PC + 2;
					return GetIndexedAddress(base);
				}
			}
	}
}

template<int size>
uint32_t MdCpu::ReadImmediate()
{
	if constexpr(size == 1) {
		return ReadExt() & 0xFF;
	} else if constexpr(size == 2) {
		return ReadExt();
	} else {
		uint32_t hi = ReadExt();
		return (hi << 16) | ReadExt();
	}
}

template<int size>
uint32_t MdCpu::ReadEa(uint8_t mode, uint8_t reg)
{
	switch(mode) {
		case 0: return _state.D[reg] & SizeMask<size>();
		case 1: return _state.A[reg] & SizeMask<size>();
		case 7:
			if(reg == 4) {
				return ReadImmediate<size>();
			}
			[[fallthrough]];
		default:
			_eaAddr = GetEaAddress<size>(mode, reg);
			return Read<size>(_eaAddr);
	}
}

template<int size>
uint32_t MdCpu::SignExtend(uint32_t value)
{
	if constexpr(size == 1) {
		return (uint32_t)(int8_t)value;
	} else if constexpr(size == 2) {
		return (uint32_t)(int16_t)value;
	} else {
		return value;
	}
}

template<int size>
void MdCpu::SetDataReg(uint8_t reg, uint32_t value)
{
	_state.D[reg] = (_state.D[reg] & ~SizeMask<size>()) | (value & SizeMask<size>());
}

template<int size>
void MdCpu::SetLogicalFlags(uint32_t result)
{
	result &= SizeMask<size>();
	uint16_t sr = _state.SR & ~(MdCpuFlags::Negative | MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(result == 0) {
		sr |= MdCpuFlags::Zero;
	}
	if(result & SignBit<size>()) {
		sr |= MdCpuFlags::Negative;
	}
	_state.SR = sr;
}

template<int size>
uint32_t MdCpu::Add(uint32_t src, uint32_t dst, bool withExtend)
{
	constexpr uint32_t mask = SizeMask<size>();
	constexpr uint32_t msb = SignBit<size>();
	src &= mask;
	dst &= mask;
	uint64_t result64 = (uint64_t)dst + src + (withExtend && CheckFlag(MdCpuFlags::Extend) ? 1 : 0);
	uint32_t result = (uint32_t)result64 & mask;
	bool carry = (result64 >> (size * 8)) & 0x01;

	uint16_t sr = _state.SR & ~(MdCpuFlags::Extend | MdCpuFlags::Negative | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(withExtend) {
		if(result) {
			sr &= ~MdCpuFlags::Zero;
		}
	} else {
		sr &= ~MdCpuFlags::Zero;
		if(result == 0) {
			sr |= MdCpuFlags::Zero;
		}
	}
	if(result & msb) {
		sr |= MdCpuFlags::Negative;
	}
	if((src ^ result) & (dst ^ result) & msb) {
		sr |= MdCpuFlags::Overflow;
	}
	if(carry) {
		sr |= MdCpuFlags::Carry | MdCpuFlags::Extend;
	}
	_state.SR = sr;
	return result;
}

template<int size>
uint32_t MdCpu::Sub(uint32_t src, uint32_t dst, bool withExtend, bool setExtend)
{
	constexpr uint32_t mask = SizeMask<size>();
	constexpr uint32_t msb = SignBit<size>();
	src &= mask;
	dst &= mask;
	uint64_t result64 = (uint64_t)dst - src - (withExtend && CheckFlag(MdCpuFlags::Extend) ? 1 : 0);
	uint32_t result = (uint32_t)result64 & mask;
	bool carry = (result64 >> (size * 8)) & 0x01;

	uint16_t sr = _state.SR & ~(MdCpuFlags::Negative | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(setExtend) {
		sr &= ~MdCpuFlags::Extend;
	}
	if(withExtend) {
		if(result) {
			sr &= ~MdCpuFlags::Zero;
		}
	} else {
		sr &= ~MdCpuFlags::Zero;
		if(result == 0) {
			sr |= MdCpuFlags::Zero;
		}
	}
	if(result & msb) {
		sr |= MdCpuFlags::Negative;
	}
	if((src ^ dst) & (result ^ dst) & msb) {
		sr |= MdCpuFlags::Overflow;
	}
	if(carry) {
		sr |= MdCpuFlags::Carry | (setExtend ? MdCpuFlags::Extend : 0);
	}
	_state.SR = sr;
	return result;
}

template<int size, int op>
void MdCpu::ArithEaToDn()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint8_t dn = (_opCode >> 9) & 0x07;

	uint32_t src = ReadEa<size>(mode, reg);
	uint32_t dst = _state.D[dn];
	uint32_t result;
	switch(op) {
		case MdArithOp::Add: result = Add<size>(src, dst, false); break;
		case MdArithOp::Sub: result = Sub<size>(src, dst, false); break;
		case MdArithOp::And: result = src & dst; SetLogicalFlags<size>(result); break;
		default: result = src | dst; SetLogicalFlags<size>(result); break;
	}
	SetDataReg<size>(dn, result);

	Prefetch();
	if constexpr(size == 4) {
		Idle((mode <= 1 || (mode == 7 && reg == 4)) ? 4 : 2);
	}
}

template<int size, int op>
void MdCpu::ArithDnToEa()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t src = _state.D[(_opCode >> 9) & 0x07];

	auto calc = [=](uint32_t dst) -> uint32_t {
		uint32_t result;
		switch(op) {
			case MdArithOp::Add: result = Add<size>(src, dst, false); break;
			case MdArithOp::Sub: result = Sub<size>(src, dst, false); break;
			case MdArithOp::And: result = src & dst; SetLogicalFlags<size>(result); break;
			case MdArithOp::Or: result = src | dst; SetLogicalFlags<size>(result); break;
			default: result = src ^ dst; SetLogicalFlags<size>(result); break;
		}
		return result;
	};

	if(mode == 0) {
		//EOR Dn, Dn
		SetDataReg<size>(reg, calc(_state.D[reg]));
		Prefetch();
		if constexpr(size == 4) {
			Idle(4);
		}
		return;
	}

	uint32_t addr = GetEaAddress<size>(mode, reg);
	uint32_t result = calc(Read<size>(addr));
	Prefetch();
	WriteReversed<size>(addr, result);
}

template<int size, int op>
void MdCpu::ArithImmediate()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t src = ReadImmediate<size>();

	auto calc = [=](uint32_t dst) -> uint32_t {
		uint32_t result;
		switch(op) {
			case MdArithOp::Add: result = Add<size>(src, dst, false); break;
			case MdArithOp::Sub: result = Sub<size>(src, dst, false); break;
			case MdArithOp::Cmp: result = dst; Sub<size>(src, dst, false, false); break;
			case MdArithOp::And: result = src & dst; SetLogicalFlags<size>(result); break;
			case MdArithOp::Or: result = src | dst; SetLogicalFlags<size>(result); break;
			default: result = src ^ dst; SetLogicalFlags<size>(result); break;
		}
		return result;
	};

	if(mode == 0) {
		SetDataReg<size>(reg, calc(_state.D[reg]));
		Prefetch();
		if constexpr(size == 4) {
			Idle(op == MdArithOp::Cmp ? 2 : 4);
		}
		return;
	}

	uint32_t addr = GetEaAddress<size>(mode, reg);
	uint32_t result = calc(Read<size>(addr));
	Prefetch();
	if constexpr(op != MdArithOp::Cmp) {
		WriteReversed<size>(addr, result);
	}
}

template<int size, bool isSub>
void MdCpu::ArithQuick()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t src = (((_opCode >> 9) - 1) & 0x07) + 1;

	if(mode == 0) {
		SetDataReg<size>(reg, isSub ? Sub<size>(src, _state.D[reg], false) : Add<size>(src, _state.D[reg], false));
		Prefetch();
		if constexpr(size == 4) {
			Idle(4);
		}
	} else if(mode == 1) {
		_state.A[reg] = isSub ? (_state.A[reg] - src) : (_state.A[reg] + src);
		Prefetch();
		Idle(4);
	} else {
		uint32_t addr = GetEaAddress<size>(mode, reg);
		uint32_t dst = Read<size>(addr);
		uint32_t result = isSub ? Sub<size>(src, dst, false) : Add<size>(src, dst, false);
		Prefetch();
		WriteReversed<size>(addr, result);
	}
}

template<int size, bool isSub>
void MdCpu::ArithAddress()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint8_t an = (_opCode >> 9) & 0x07;

	uint32_t src = SignExtend<size>(ReadEa<size>(mode, reg));
	_state.A[an] = isSub ? (_state.A[an] - src) : (_state.A[an] + src);

	Prefetch();
	if constexpr(size == 2) {
		Idle(4);
	} else {
		Idle((mode <= 1 || (mode == 7 && reg == 4)) ? 4 : 2);
	}
}

template<int size>
void MdCpu::Cmp()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t src = ReadEa<size>(mode, reg);
	Sub<size>(src, _state.D[(_opCode >> 9) & 0x07], false, false);
	Prefetch();
	if constexpr(size == 4) {
		Idle(2);
	}
}

template<int size>
void MdCpu::Cmpa()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t src = SignExtend<size>(ReadEa<size>(mode, reg));
	Sub<4>(src, _state.A[(_opCode >> 9) & 0x07], false, false);
	Prefetch();
	Idle(2);
}

template<int size>
void MdCpu::Cmpm()
{
	uint8_t ry = _opCode & 0x07;
	uint8_t rx = (_opCode >> 9) & 0x07;
	uint32_t src = Read<size>(GetEaAddress<size>(3, ry));
	uint32_t dst = Read<size>(GetEaAddress<size>(3, rx));
	Sub<size>(src, dst, false, false);
	Prefetch();
}

template<int size, bool isSub>
void MdCpu::ArithExtend()
{
	uint8_t ry = _opCode & 0x07;
	uint8_t rx = (_opCode >> 9) & 0x07;

	if(!(_opCode & 0x08)) {
		uint32_t src = _state.D[ry];
		uint32_t dst = _state.D[rx];
		SetDataReg<size>(rx, isSub ? Sub<size>(src, dst, true) : Add<size>(src, dst, true));
		Prefetch();
		if constexpr(size == 4) {
			Idle(4);
		}
		return;
	}

	Idle(2);
	if constexpr(size == 4) {
		_state.A[ry] -= 2;
		uint32_t srcLo = ReadWord(_state.A[ry]);
		_state.A[ry] -= 2;
		uint32_t srcHi = ReadWord(_state.A[ry]);
		_state.A[rx] -= 2;
		uint32_t dstLo = ReadWord(_state.A[rx]);
		_state.A[rx] -= 2;
		uint32_t dstHi = ReadWord(_state.A[rx]);

		uint32_t src = (srcHi << 16) | srcLo;
		uint32_t dst = (dstHi << 16) | dstLo;
		uint32_t result = isSub ? Sub<4>(src, dst, true) : Add<4>(src, dst, true);
		WriteWord(_state.A[rx] + 2, (uint16_t)result);
		Prefetch();
		WriteWord(_state.A[rx], (uint16_t)(result >> 16));
	} else {
		uint32_t src = Read<size>(GetEaAddress<size, true>(4, ry));
		uint32_t dstAddr = GetEaAddress<size, true>(4, rx);
		uint32_t dst = Read<size>(dstAddr);
		uint32_t result = isSub ? Sub<size>(src, dst, true) : Add<size>(src, dst, true);
		Prefetch();
		Write<size>(dstAddr, result);
	}
}

template<int size>
void MdCpu::Move()
{
	uint8_t srcMode = (_opCode >> 3) & 0x07;
	uint8_t srcReg = _opCode & 0x07;
	uint8_t dstMode = (_opCode >> 6) & 0x07;
	uint8_t dstReg = (_opCode >> 9) & 0x07;

	uint32_t value = ReadEa<size>(srcMode, srcReg);
	SetLogicalFlags<size>(value);

	switch(dstMode) {
		case 0:
			SetDataReg<size>(dstReg, value);
			Prefetch();
			break;

		case 3: {
			uint32_t addr = _state.A[dstReg];
			Write<size>(addr, value);
			_state.A[dstReg] += (size == 1 && dstReg == 7) ? 2 : size;
			Prefetch();
			break;
		}

		case 4:
			Prefetch();
			if constexpr(size == 4) {
				_state.A[dstReg] -= 2;
				WriteWord(_state.A[dstReg], (uint16_t)value);
				_state.A[dstReg] -= 2;
				WriteWord(_state.A[dstReg], (uint16_t)(value >> 16));
			} else {
				Write<size>(GetEaAddress<size, true>(4, dstReg), value);
			}
			break;

		case 7:
			if(dstReg == 1 && srcMode >= 2 && !(srcMode == 7 && srcReg == 4)) {
				//MOVE <mem>, (xxx).l - the write occurs before the second extension word is consumed
				uint32_t hi = ReadExt();
				uint32_t addr = (hi << 16) | _state.IRC;
				Write<size>(addr, value);
				ReadExt();
				Prefetch();
				break;
			}
			[[fallthrough]];

		default: {
			uint32_t addr = GetEaAddress<size, true>(dstMode, dstReg);
			Write<size>(addr, value);
			Prefetch();
			break;
		}
	}
}

template<int size>
void MdCpu::Movea()
{
	uint8_t srcMode = (_opCode >> 3) & 0x07;
	uint8_t srcReg = _opCode & 0x07;
	_state.A[(_opCode >> 9) & 0x07] = SignExtend<size>(ReadEa<size>(srcMode, srcReg));
	Prefetch();
}

void MdCpu::Moveq()
{
	uint32_t value = (uint32_t)(int8_t)_opCode;
	_state.D[(_opCode >> 9) & 0x07] = value;
	SetLogicalFlags<4>(value);
	Prefetch();
}

template<int size, int op>
void MdCpu::UnaryOp()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;

	auto calc = [=](uint32_t dst) -> uint32_t {
		switch(op) {
			case MdUnaryOp::Negx: return Sub<size>(dst, 0, true);
			case MdUnaryOp::Clr: SetLogicalFlags<size>(0); return 0;
			case MdUnaryOp::Neg: return Sub<size>(dst, 0, false);
			default: {
				uint32_t result = ~dst & SizeMask<size>();
				SetLogicalFlags<size>(result);
				return result;
			}
		}
	};

	if(mode == 0) {
		SetDataReg<size>(reg, calc(_state.D[reg]));
		Prefetch();
		if constexpr(size == 4) {
			Idle(2);
		}
		return;
	}

	uint32_t addr = GetEaAddress<size>(mode, reg);
	uint32_t result = calc(Read<size>(addr));
	Prefetch();
	WriteReversed<size>(addr, result);
}

template<int size>
void MdCpu::Tst()
{
	SetLogicalFlags<size>(ReadEa<size>((_opCode >> 3) & 0x07, _opCode & 0x07));
	Prefetch();
}

void MdCpu::Tas()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	if(mode == 0) {
		SetLogicalFlags<1>(_state.D[reg]);
		_state.D[reg] |= 0x80;
		Prefetch();
		return;
	}

	uint32_t addr = GetEaAddress<1>(mode, reg);
	uint8_t value = ReadByte(addr);
	SetLogicalFlags<1>(value);
	Idle(2);
#ifdef DUMMYCPU
	LogMemoryOperation(addr & 0xFFFFFF, value | 0x80, MemoryOperationType::Write, MemoryType::MdMemory);
#else
	_memoryManager->WriteTas(addr, value | 0x80);
#endif
	Prefetch();
}

void MdCpu::Scc()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	bool cond = CheckCondition((_opCode >> 8) & 0x0F);
	uint8_t value = cond ? 0xFF : 0x00;

	if(mode == 0) {
		SetDataReg<1>(reg, value);
		Prefetch();
		if(cond) {
			Idle(2);
		}
		return;
	}

	uint32_t addr = GetEaAddress<1>(mode, reg);
	ReadByte(addr);
	Prefetch();
	WriteByte(addr, value);
}

void MdCpu::Dbcc()
{
	uint8_t reg = _opCode & 0x07;
	if(CheckCondition((_opCode >> 8) & 0x0F)) {
		Idle(4);
		ReadExt();
		Prefetch();
		return;
	}

	uint16_t counter = (uint16_t)_state.D[reg] - 1;
	SetDataReg<2>(reg, counter);
	if(counter != 0xFFFF) {
		uint32_t target = _state.PC + 2 + (int16_t)_state.IRC;
		Idle(2);
		FillQueue(target);
	} else {
		Idle(2);
		ReadProgramWord(_state.PC + 2 + (int16_t)_state.IRC);
		ReadExt();
		Prefetch();
	}
}

void MdCpu::Bcc()
{
	int8_t disp = (int8_t)_opCode;
	if(CheckCondition((_opCode >> 8) & 0x0F)) {
		uint32_t target = _state.PC + 2 + (disp == 0 ? (int16_t)_state.IRC : disp);
		Idle(2);
		FillQueue(target);
	} else {
		Idle(4);
		if(disp == 0) {
			ReadExt();
		}
		Prefetch();
	}
}

void MdCpu::Bsr()
{
	int8_t disp = (int8_t)_opCode;
	uint32_t target = _state.PC + 2 + (disp == 0 ? (int16_t)_state.IRC : disp);
	uint32_t returnAddr = _state.PC + (disp == 0 ? 4 : 2);
	Idle(2);
	_state.A[7] -= 4;
	WriteLong(_state.A[7], returnAddr);
	FillQueue(target);
}

//BCD flag behavior (including the undocumented N/V flags) based on Flamewing's research on real hardware
uint8_t MdCpu::AddBcd(uint8_t src, uint8_t dst)
{
	uint32_t x = CheckFlag(MdCpuFlags::Extend) ? 1 : 0;
	uint32_t ss = dst + src + x;
	uint32_t bc = ((dst & src) | (~ss & dst) | (~ss & src)) & 0x88;
	uint32_t dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
	uint32_t corf = (bc | dc) - ((bc | dc) >> 2);
	uint32_t rr = ss + corf;
	SetBcdFlags(rr, (bc | (ss & ~rr)) & 0x80, (~ss & rr) & 0x80);
	return (uint8_t)rr;
}

uint8_t MdCpu::SubBcd(uint8_t src, uint8_t dst)
{
	uint32_t x = CheckFlag(MdCpuFlags::Extend) ? 1 : 0;
	uint32_t dd = dst - src - x;
	uint32_t bc = ((~dst & src) | (dd & ~dst) | (dd & src)) & 0x88;
	uint32_t corf = bc - (bc >> 2);
	uint32_t rr = dd - corf;
	SetBcdFlags(rr, (bc | (~dd & rr)) & 0x80, (dd & ~rr) & 0x80);
	return (uint8_t)rr;
}

void MdCpu::SetBcdFlags(uint32_t result, bool carry, bool overflow)
{
	uint16_t sr = _state.SR & ~(MdCpuFlags::Extend | MdCpuFlags::Negative | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(result & 0xFF) {
		sr &= ~MdCpuFlags::Zero;
	}
	if(result & 0x80) {
		sr |= MdCpuFlags::Negative;
	}
	if(overflow) {
		sr |= MdCpuFlags::Overflow;
	}
	if(carry) {
		sr |= MdCpuFlags::Carry | MdCpuFlags::Extend;
	}
	_state.SR = sr;
}

template<bool isSub>
void MdCpu::Bcd()
{
	uint8_t ry = _opCode & 0x07;
	uint8_t rx = (_opCode >> 9) & 0x07;

	auto calc = [=](uint8_t src, uint8_t dst) -> uint8_t {
		return isSub ? SubBcd(src, dst) : AddBcd(src, dst);
	};

	if(!(_opCode & 0x08)) {
		SetDataReg<1>(rx, calc((uint8_t)_state.D[ry], (uint8_t)_state.D[rx]));
		Prefetch();
		Idle(2);
	} else {
		Idle(2);
		uint8_t src = ReadByte(GetEaAddress<1, true>(4, ry));
		uint32_t dstAddr = GetEaAddress<1, true>(4, rx);
		uint8_t dst = ReadByte(dstAddr);
		uint8_t result = calc(src, dst);
		Prefetch();
		WriteByte(dstAddr, result);
	}
}

void MdCpu::Nbcd()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;

	if(mode == 0) {
		SetDataReg<1>(reg, SubBcd((uint8_t)_state.D[reg], 0));
		Prefetch();
		Idle(2);
		return;
	}

	uint32_t addr = GetEaAddress<1>(mode, reg);
	uint8_t result = SubBcd(ReadByte(addr), 0);
	Prefetch();
	WriteByte(addr, result);
}

template<int op, bool isImmediate>
void MdCpu::BitOp()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t bit = isImmediate ? (ReadExt() & 0xFF) : _state.D[(_opCode >> 9) & 0x07];

	auto calc = [=](uint32_t value, uint32_t mask) -> uint32_t {
		SetFlag(MdCpuFlags::Zero, (value & mask) == 0);
		switch(op) {
			default:
			case 0: return value;
			case 1: return value ^ mask;
			case 2: return value & ~mask;
			case 3: return value | mask;
		}
	};

	if(mode == 0) {
		bit &= 0x1F;
		_state.D[reg] = calc(_state.D[reg], 1 << bit);
		Prefetch();
		switch(op) {
			case 0: Idle(2); break;
			case 2: Idle(bit < 16 ? 4 : 6); break;
			default: Idle(bit < 16 ? 2 : 4); break;
		}
		return;
	}

	bit &= 0x07;
	if constexpr(op == 0) {
		calc(ReadEa<1>(mode, reg), 1 << bit);
		Prefetch();
		if(mode == 7 && reg == 4) {
			Idle(2);
		}
	} else {
		uint32_t addr = GetEaAddress<1>(mode, reg);
		uint8_t result = (uint8_t)calc(ReadByte(addr), 1 << bit);
		Prefetch();
		WriteByte(addr, result);
	}
}

template<int size, bool toMemory>
void MdCpu::Movep()
{
	uint8_t dn = (_opCode >> 9) & 0x07;
	uint32_t addr = _state.A[_opCode & 0x07] + (int16_t)ReadExt();

	if constexpr(toMemory) {
		uint32_t value = _state.D[dn];
		for(int i = size - 1; i >= 0; i--) {
			WriteByte(addr, (uint8_t)(value >> (i * 8)));
			addr += 2;
		}
	} else {
		uint32_t value = 0;
		for(int i = 0; i < size; i++) {
			value = (value << 8) | ReadByte(addr);
			addr += 2;
		}
		SetDataReg<size>(dn, value);
	}
	Prefetch();
}

template<int size, bool toMemory>
void MdCpu::Movem()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint16_t mask = ReadExt();

	if constexpr(toMemory) {
		if(mode == 4) {
			uint32_t addr = _state.A[reg];
			for(int i = 0; i < 16; i++) {
				if(mask & (1 << i)) {
					uint32_t value = i < 8 ? _state.A[7 - i] : _state.D[15 - i];
					if constexpr(size == 4) {
						addr -= 2;
						WriteWord(addr, (uint16_t)value);
						addr -= 2;
						WriteWord(addr, (uint16_t)(value >> 16));
					} else {
						addr -= 2;
						WriteWord(addr, (uint16_t)value);
					}
				}
			}
			_state.A[reg] = addr;
		} else {
			uint32_t addr = GetEaAddress<size>(mode, reg);
			for(int i = 0; i < 16; i++) {
				if(mask & (1 << i)) {
					uint32_t value = i < 8 ? _state.D[i] : _state.A[i - 8];
					Write<size>(addr, value);
					addr += size;
				}
			}
		}
	} else {
		uint32_t addr = mode == 3 ? _state.A[reg] : GetEaAddress<size>(mode, reg);
		if(mode == 3 && (addr & 0x01)) {
			_state.A[reg] = addr + 2;
		}
		for(int i = 0; i < 16; i++) {
			if(mask & (1 << i)) {
				uint32_t value = SignExtend<size>(Read<size>(addr));
				if(i < 8) {
					_state.D[i] = value;
				} else {
					_state.A[i - 8] = value;
				}
				addr += size;
			}
		}
		ReadWord(addr);
		if(mode == 3) {
			_state.A[reg] = addr;
		}
	}
	Prefetch();
}

void MdCpu::Mulu()
{
	uint16_t src = (uint16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	uint8_t dn = (_opCode >> 9) & 0x07;
	uint32_t result = (uint32_t)src * (uint16_t)_state.D[dn];
	_state.D[dn] = result;
	SetLogicalFlags<4>(result);

	int bits = 0;
	for(uint16_t v = src; v; v >>= 1) {
		bits += v & 0x01;
	}

	Prefetch();
	Idle(34 + bits * 2);
}

void MdCpu::Muls()
{
	uint16_t src = (uint16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	uint8_t dn = (_opCode >> 9) & 0x07;
	uint32_t result = (uint32_t)((int32_t)(int16_t)src * (int16_t)_state.D[dn]);
	_state.D[dn] = result;
	SetLogicalFlags<4>(result);

	int bits = 0;
	for(uint16_t v = (src ^ (src << 1)) & 0xFFFF; v; v >>= 1) {
		bits += v & 0x01;
	}

	Prefetch();
	Idle(34 + bits * 2);
}

void MdCpu::Divu()
{
	uint16_t divisor = (uint16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	uint8_t dn = (_opCode >> 9) & 0x07;
	uint32_t dividend = _state.D[dn];

	if(divisor == 0) {
		_state.SR &= ~(MdCpuFlags::Negative | MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
		Idle(4);
		ProcessException(5, _state.PC + 2, 4);
		return;
	}

	if((dividend >> 16) >= divisor) {
		Idle(6);
		_state.SR = (_state.SR & ~MdCpuFlags::Carry) | MdCpuFlags::Overflow;
		Prefetch();
		return;
	}

	//Cycle timing algorithm by Jorge Cwik
	uint32_t cycles = 38;
	uint32_t hdivisor = (uint32_t)divisor << 16;
	uint32_t value = dividend;
	for(int i = 0; i < 15; i++) {
		uint32_t temp = value;
		value <<= 1;
		if(temp & 0x80000000) {
			value -= hdivisor;
		} else {
			cycles += 2;
			if(value >= hdivisor) {
				value -= hdivisor;
				cycles--;
			}
		}
	}

	uint16_t quotient = (uint16_t)(dividend / divisor);
	uint16_t remainder = (uint16_t)(dividend % divisor);
	_state.D[dn] = ((uint32_t)remainder << 16) | quotient;
	SetLogicalFlags<2>(quotient);

	Idle(cycles * 2 - 4);
	Prefetch();
}

void MdCpu::Divs()
{
	int16_t divisor = (int16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	uint8_t dn = (_opCode >> 9) & 0x07;
	int32_t dividend = (int32_t)_state.D[dn];

	if(divisor == 0) {
		_state.SR &= ~(MdCpuFlags::Negative | MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
		Idle(4);
		ProcessException(5, _state.PC + 2, 4);
		return;
	}

	//Cycle timing algorithm by Jorge Cwik
	uint32_t cycles = 6;
	if(dividend < 0) {
		cycles++;
	}

	uint32_t absDividend = dividend < 0 ? (uint32_t)-(int64_t)dividend : (uint32_t)dividend;
	uint16_t absDivisor = divisor < 0 ? (uint16_t)-divisor : (uint16_t)divisor;

	if((absDividend >> 16) >= absDivisor) {
		cycles += 2;
		Idle(cycles * 2 - 4);
		_state.SR = (_state.SR & ~MdCpuFlags::Carry) | MdCpuFlags::Overflow;
		Prefetch();
		return;
	}

	uint32_t absQuotient = absDividend / absDivisor;
	cycles += 55;
	if(divisor >= 0) {
		if(dividend >= 0) {
			cycles--;
		} else {
			cycles++;
		}
	}

	uint32_t q = absQuotient;
	for(int i = 0; i < 15; i++) {
		if(!(q & 0x8000)) {
			cycles++;
		}
		q <<= 1;
	}

	int32_t quotient = dividend / divisor;
	int32_t remainder = dividend % divisor;
	Idle(cycles * 2 - 4);

	if(quotient < -32768 || quotient > 32767) {
		_state.SR = (_state.SR & ~MdCpuFlags::Carry) | MdCpuFlags::Overflow;
	} else {
		_state.D[dn] = ((uint32_t)(uint16_t)remainder << 16) | (uint16_t)quotient;
		SetLogicalFlags<2>((uint16_t)quotient);
	}
	Prefetch();
}

template<int size, int type, bool left>
void MdCpu::ShiftRegister()
{
	constexpr uint32_t mask = SizeMask<size>();
	constexpr uint32_t msb = SignBit<size>();

	uint8_t reg = _opCode & 0x07;
	uint8_t count = (_opCode & 0x20) ? (_state.D[(_opCode >> 9) & 0x07] & 0x3F) : ((((_opCode >> 9) - 1) & 0x07) + 1);

	uint32_t value = _state.D[reg] & mask;
	bool carry = false;
	bool overflow = false;
	bool x = CheckFlag(MdCpuFlags::Extend);

	for(int i = 0; i < count; i++) {
		switch(type) {
			case MdShiftType::As:
				if constexpr(left) {
					carry = value & msb;
					uint32_t newValue = (value << 1) & mask;
					if((newValue ^ value) & msb) {
						overflow = true;
					}
					value = newValue;
				} else {
					carry = value & 0x01;
					value = (value >> 1) | (value & msb);
				}
				break;

			case MdShiftType::Ls:
				if constexpr(left) {
					carry = value & msb;
					value = (value << 1) & mask;
				} else {
					carry = value & 0x01;
					value >>= 1;
				}
				break;

			case MdShiftType::Rox:
				if constexpr(left) {
					carry = value & msb;
					value = ((value << 1) | (x ? 1 : 0)) & mask;
				} else {
					carry = value & 0x01;
					value = (value >> 1) | (x ? msb : 0);
				}
				x = carry;
				break;

			case MdShiftType::Ro:
				if constexpr(left) {
					carry = value & msb;
					value = ((value << 1) | (carry ? 1 : 0)) & mask;
				} else {
					carry = value & 0x01;
					value = (value >> 1) | (carry ? msb : 0);
				}
				break;
		}
	}

	uint16_t sr = _state.SR & ~(MdCpuFlags::Negative | MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(value == 0) {
		sr |= MdCpuFlags::Zero;
	}
	if(value & msb) {
		sr |= MdCpuFlags::Negative;
	}
	if(overflow) {
		sr |= MdCpuFlags::Overflow;
	}
	if(count == 0) {
		if constexpr(type == MdShiftType::Rox) {
			if(CheckFlag(MdCpuFlags::Extend)) {
				sr |= MdCpuFlags::Carry;
			}
		}
	} else {
		if(carry) {
			sr |= MdCpuFlags::Carry;
		}
		if constexpr(type != MdShiftType::Ro) {
			sr = carry ? (sr | MdCpuFlags::Extend) : (sr & ~MdCpuFlags::Extend);
		}
	}
	_state.SR = sr;

	SetDataReg<size>(reg, value);
	Prefetch();
	Idle((size == 4 ? 4 : 2) + count * 2);
}

template<int type, bool left>
void MdCpu::ShiftMemory()
{
	uint32_t addr = GetEaAddress<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	uint32_t value = ReadWord(addr);
	bool carry;
	bool x = CheckFlag(MdCpuFlags::Extend);
	bool overflow = false;

	switch(type) {
		default:
		case MdShiftType::As:
			if constexpr(left) {
				carry = value & 0x8000;
				uint32_t newValue = (value << 1) & 0xFFFF;
				overflow = ((newValue ^ value) & 0x8000) != 0;
				value = newValue;
			} else {
				carry = value & 0x01;
				value = (value >> 1) | (value & 0x8000);
			}
			break;

		case MdShiftType::Ls:
			if constexpr(left) {
				carry = value & 0x8000;
				value = (value << 1) & 0xFFFF;
			} else {
				carry = value & 0x01;
				value >>= 1;
			}
			break;

		case MdShiftType::Rox:
			if constexpr(left) {
				carry = value & 0x8000;
				value = ((value << 1) | (x ? 1 : 0)) & 0xFFFF;
			} else {
				carry = value & 0x01;
				value = (value >> 1) | (x ? 0x8000 : 0);
			}
			break;

		case MdShiftType::Ro:
			if constexpr(left) {
				carry = value & 0x8000;
				value = ((value << 1) | (carry ? 1 : 0)) & 0xFFFF;
			} else {
				carry = value & 0x01;
				value = (value >> 1) | (carry ? 0x8000 : 0);
			}
			break;
	}

	uint16_t sr = _state.SR & ~(MdCpuFlags::Negative | MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(value == 0) {
		sr |= MdCpuFlags::Zero;
	}
	if(value & 0x8000) {
		sr |= MdCpuFlags::Negative;
	}
	if(overflow) {
		sr |= MdCpuFlags::Overflow;
	}
	if(carry) {
		sr |= MdCpuFlags::Carry;
	}
	if constexpr(type != MdShiftType::Ro) {
		sr = carry ? (sr | MdCpuFlags::Extend) : (sr & ~MdCpuFlags::Extend);
	}
	_state.SR = sr;

	Prefetch();
	WriteWord(addr, (uint16_t)value);
}

void MdCpu::Swap()
{
	uint8_t reg = _opCode & 0x07;
	uint32_t value = (_state.D[reg] >> 16) | (_state.D[reg] << 16);
	_state.D[reg] = value;
	SetLogicalFlags<4>(value);
	Prefetch();
}

void MdCpu::Ext()
{
	uint8_t reg = _opCode & 0x07;
	if(_opCode & 0x40) {
		_state.D[reg] = (uint32_t)(int16_t)_state.D[reg];
		SetLogicalFlags<4>(_state.D[reg]);
	} else {
		SetDataReg<2>(reg, (uint16_t)(int8_t)_state.D[reg]);
		SetLogicalFlags<2>(_state.D[reg]);
	}
	Prefetch();
}

void MdCpu::Exg()
{
	uint8_t rx = (_opCode >> 9) & 0x07;
	uint8_t ry = _opCode & 0x07;
	switch((_opCode >> 3) & 0x1F) {
		case 0x08: std::swap(_state.D[rx], _state.D[ry]); break;
		case 0x09: std::swap(_state.A[rx], _state.A[ry]); break;
		default: std::swap(_state.D[rx], _state.A[ry]); break;
	}
	Prefetch();
	Idle(2);
}

void MdCpu::Lea()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t addr;
	if(mode == 6 || (mode == 7 && reg == 3)) {
		Idle(2);
		addr = GetIndexedAddress(mode == 6 ? _state.A[reg] : (_state.PC + 2));
		Idle(2);
	} else {
		addr = GetEaAddress<4>(mode, reg);
	}
	_state.A[(_opCode >> 9) & 0x07] = addr;
	Prefetch();
}

void MdCpu::Pea()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t addr;
	if(mode == 6 || (mode == 7 && reg == 3)) {
		Idle(2);
		addr = GetIndexedAddress(mode == 6 ? _state.A[reg] : (_state.PC + 2));
		Idle(2);
	} else {
		addr = GetEaAddress<4>(mode, reg);
	}

	if(mode == 7 && (reg == 0 || reg == 1)) {
		_state.A[7] -= 4;
		WriteLong(_state.A[7], addr);
		Prefetch();
	} else {
		Prefetch();
		_state.A[7] -= 4;
		WriteLong(_state.A[7], addr);
	}
}

void MdCpu::Link()
{
	uint8_t reg = _opCode & 0x07;
	int16_t disp = (int16_t)ReadExt();
	_state.A[7] -= 4;
	WriteLong(_state.A[7], _state.A[reg]);
	_state.A[reg] = _state.A[7];
	_state.A[7] += disp;
	Prefetch();
}

void MdCpu::Unlk()
{
	uint8_t reg = _opCode & 0x07;
	_state.A[7] = _state.A[reg];
	uint32_t value = ReadLong(_state.A[7]);
	_state.A[7] += 4;
	_state.A[reg] = value;
	Prefetch();
}

void MdCpu::Jmp()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t ext = _state.IRC;
	uint32_t target;
	switch(mode) {
		case 2: target = _state.A[reg]; break;
		case 5: Idle(2); target = _state.A[reg] + (int16_t)ext; break;
		case 6: Idle(6); target = _state.A[reg] + (int8_t)ext; break;
		default:
			switch(reg) {
				case 0: Idle(2); target = (uint32_t)(int16_t)ext; break;
				case 1: ReadExt(); target = (ext << 16) | _state.IRC; break;
				case 2: Idle(2); target = _state.PC + 2 + (int16_t)ext; break;
				default: Idle(6); target = _state.PC + 2 + (int8_t)ext; break;
			}
			break;
	}

	if(mode == 6 || (mode == 7 && reg == 3)) {
		uint32_t index = (ext & 0x8000) ? _state.A[(ext >> 12) & 0x07] : _state.D[(ext >> 12) & 0x07];
		if(!(ext & 0x800)) {
			index = (uint32_t)(int16_t)index;
		}
		target += index;
	}

	FillQueue(target);
}

void MdCpu::Jsr()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	uint32_t ext = _state.IRC;
	uint32_t target;
	switch(mode) {
		case 2: target = _state.A[reg]; break;
		case 5: Idle(2); target = _state.A[reg] + (int16_t)ext; break;
		case 6: Idle(6); target = _state.A[reg] + (int8_t)ext; break;
		default:
			switch(reg) {
				case 0: Idle(2); target = (uint32_t)(int16_t)ext; break;
				case 1: ReadExt(); target = (ext << 16) | _state.IRC; break;
				case 2: Idle(2); target = _state.PC + 2 + (int16_t)ext; break;
				default: Idle(6); target = _state.PC + 2 + (int8_t)ext; break;
			}
			break;
	}

	if(mode == 6 || (mode == 7 && reg == 3)) {
		uint32_t index = (ext & 0x8000) ? _state.A[(ext >> 12) & 0x07] : _state.D[(ext >> 12) & 0x07];
		if(!(ext & 0x800)) {
			index = (uint32_t)(int16_t)index;
		}
		target += index;
	}

	uint32_t returnAddr = _state.PC + (mode == 2 ? 2 : 4);
	if(target & 0x01) {
		_state.PC = target - 4;
		AddressError(target, true, true);
	}

	uint16_t ir = BusReadWord(target, MemoryOperationType::ExecOpCode);
	_state.A[7] -= 4;
	WriteLong(_state.A[7], returnAddr);
	_state.PC = target;
	_state.IR = ir;
	_state.IRC = ReadProgramWord(target + 2);
}

void MdCpu::Rts()
{
	uint32_t pc = ReadLong(_state.A[7]);
	_state.A[7] += 4;
	FillQueue(pc);
}

void MdCpu::Rte()
{
	if(!CheckPrivilege()) {
		return;
	}

	uint32_t sp = _state.A[7];
	uint32_t pcHi = ReadWord(sp + 2);
	uint16_t sr = ReadWord(sp);
	uint32_t pcLo = ReadWord(sp + 4);
	_state.A[7] = sp + 6;
	SetSR(sr);
	FillQueue((pcHi << 16) | pcLo);
}

void MdCpu::Rtr()
{
	uint32_t sp = _state.A[7];
	uint32_t pcHi = ReadWord(sp + 2);
	uint16_t ccr = ReadWord(sp);
	uint32_t pcLo = ReadWord(sp + 4);
	_state.A[7] = sp + 6;
	SetCCR((uint8_t)ccr);
	FillQueue((pcHi << 16) | pcLo);
}

void MdCpu::Chk()
{
	int16_t bound = (int16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	int16_t value = (int16_t)_state.D[(_opCode >> 9) & 0x07];
	Prefetch();

	_state.SR &= ~(MdCpuFlags::Zero | MdCpuFlags::Overflow | MdCpuFlags::Carry);
	if(value == 0) {
		_state.SR |= MdCpuFlags::Zero;
	}

	if(value > bound) {
		SetFlag(MdCpuFlags::Negative, value < 0);
		Idle(4);
		ProcessException(6, _state.PC, 0);
	} else if(value < 0) {
		_state.SR |= MdCpuFlags::Negative;
		Idle(6);
		ProcessException(6, _state.PC, 0);
	} else {
		Idle(6);
	}
}

void MdCpu::Trap()
{
	ProcessException(32 + (_opCode & 0x0F), _state.PC + 2, 4);
}

void MdCpu::Trapv()
{
	Prefetch();
	if(CheckFlag(MdCpuFlags::Overflow)) {
		ProcessException(7, _state.PC, 0);
	}
}

void MdCpu::MoveFromSr()
{
	uint8_t mode = (_opCode >> 3) & 0x07;
	uint8_t reg = _opCode & 0x07;
	if(mode == 0) {
		SetDataReg<2>(reg, _state.SR);
		Prefetch();
		Idle(2);
		return;
	}

	uint32_t addr = GetEaAddress<2>(mode, reg);
	ReadWord(addr);
	Prefetch();
	WriteWord(addr, _state.SR);
}

template<bool toSr>
void MdCpu::MoveToSr()
{
	if constexpr(toSr) {
		if(!CheckPrivilege()) {
			return;
		}
	}

	uint16_t value = (uint16_t)ReadEa<2>((_opCode >> 3) & 0x07, _opCode & 0x07);
	Idle(4);
	if constexpr(toSr) {
		SetSR(value);
	} else {
		SetCCR((uint8_t)value);
	}
	FillQueue(_state.PC + 2);
}

template<int op, bool toSr>
void MdCpu::LogicImmediateSr()
{
	if constexpr(toSr) {
		if(!CheckPrivilege()) {
			return;
		}
	}

	uint16_t value = ReadExt();
	uint16_t current = _state.SR;
	uint16_t result;
	switch(op) {
		default:
		case 0: result = current | value; break;
		case 1: result = current & (toSr ? value : (value | 0xFF00)); break;
		case 5: result = current ^ value; break;
	}

	Idle(8);
	if constexpr(toSr) {
		SetSR(result);
	} else {
		SetCCR((uint8_t)result);
	}
	FillQueue(_state.PC + 2);
}

template<bool toUsp>
void MdCpu::MoveUsp()
{
	if(!CheckPrivilege()) {
		return;
	}

	uint8_t reg = _opCode & 0x07;
	if constexpr(toUsp) {
		_state.USP = _state.A[reg];
	} else {
		_state.A[reg] = _state.USP;
	}
	Prefetch();
}

void MdCpu::Nop()
{
	Prefetch();
}

void MdCpu::ResetInstruction()
{
	if(!CheckPrivilege()) {
		return;
	}

	Idle(4);
#ifndef DUMMYCPU
	_memoryManager->ProcessResetInstruction();
#endif
	Idle(124);
	Prefetch();
}

void MdCpu::Stop()
{
	if(!CheckPrivilege()) {
		return;
	}

	uint16_t value = ReadExt();
	SetSR(value);
	_state.PC += 2;
	_state.IR = _state.IRC;
	_state.Stopped = true;
}

void MdCpu::Illegal()
{
	_skipTrace = true;
	ProcessException(4, _state.PC, 4);
}

void MdCpu::LineA()
{
	_skipTrace = true;
	ProcessException(10, _state.PC, 4);
}

void MdCpu::LineF()
{
	_skipTrace = true;
	ProcessException(11, _state.PC, 4);
}

void MdCpu::InitOpTable()
{
	for(int i = 0; i < 0x10000; i++) {
		_opTable[i] = &MdCpu::Illegal;
	}

	for(int i = 0; i < 0x10000; i++) {
		uint16_t op = (uint16_t)i;
		uint8_t mode = (op >> 3) & 0x07;
		uint8_t reg = op & 0x07;
		uint8_t size = (op >> 6) & 0x03;
		uint8_t opMode = (op >> 6) & 0x07;
		OpFunc& f = _opTable[op];

		switch(op >> 12) {
			case 0x0:
				if((op & 0x0138) == 0x0108) {
					switch(opMode) {
						case 4: f = &MdCpu::Movep<2, false>; break;
						case 5: f = &MdCpu::Movep<4, false>; break;
						case 6: f = &MdCpu::Movep<2, true>; break;
						case 7: f = &MdCpu::Movep<4, true>; break;
					}
				} else if(op & 0x0100) {
					if(size == 0 && IsValidEa(mode, reg, MdEaMode::Data)) {
						f = &MdCpu::BitOp<0, false>;
					} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
						switch(size) {
							case 1: f = &MdCpu::BitOp<1, false>; break;
							case 2: f = &MdCpu::BitOp<2, false>; break;
							case 3: f = &MdCpu::BitOp<3, false>; break;
						}
					}
				} else if((op & 0x0F00) == 0x0800) {
					if(size == 0 && IsValidEa(mode, reg, MdEaMode::Data & ~MdEaMode::Immediate)) {
						f = &MdCpu::BitOp<0, true>;
					} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
						switch(size) {
							case 1: f = &MdCpu::BitOp<1, true>; break;
							case 2: f = &MdCpu::BitOp<2, true>; break;
							case 3: f = &MdCpu::BitOp<3, true>; break;
						}
					}
				} else {
					uint8_t immOp = (op >> 9) & 0x07;
					bool isSrCcr = (op & 0x3F) == 0x3C;
					if(isSrCcr && (immOp == 0 || immOp == 1 || immOp == 5)) {
						if(size == 0) {
							switch(immOp) {
								case 0: f = &MdCpu::LogicImmediateSr<0, false>; break;
								case 1: f = &MdCpu::LogicImmediateSr<1, false>; break;
								case 5: f = &MdCpu::LogicImmediateSr<5, false>; break;
							}
						} else if(size == 1) {
							switch(immOp) {
								case 0: f = &MdCpu::LogicImmediateSr<0, true>; break;
								case 1: f = &MdCpu::LogicImmediateSr<1, true>; break;
								case 5: f = &MdCpu::LogicImmediateSr<5, true>; break;
							}
						}
					} else if(size != 3 && IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
						#define MD_IMM_OP(sz) \
							switch(immOp) { \
								case 0: f = &MdCpu::ArithImmediate<sz, MdArithOp::Or>; break; \
								case 1: f = &MdCpu::ArithImmediate<sz, MdArithOp::And>; break; \
								case 2: f = &MdCpu::ArithImmediate<sz, MdArithOp::Sub>; break; \
								case 3: f = &MdCpu::ArithImmediate<sz, MdArithOp::Add>; break; \
								case 5: f = &MdCpu::ArithImmediate<sz, MdArithOp::Eor>; break; \
								case 6: f = &MdCpu::ArithImmediate<sz, MdArithOp::Cmp>; break; \
							}
						switch(size) {
							case 0: MD_IMM_OP(1); break;
							case 1: MD_IMM_OP(2); break;
							case 2: MD_IMM_OP(4); break;
						}
						#undef MD_IMM_OP
					}
				}
				break;

			case 0x1:
			case 0x2:
			case 0x3: {
				uint8_t dstMode = (op >> 6) & 0x07;
				uint8_t dstReg = (op >> 9) & 0x07;
				uint8_t line = op >> 12;
				uint16_t srcModes = line == 1 ? MdEaMode::Data : MdEaMode::All;
				if(!IsValidEa(mode, reg, srcModes)) {
					break;
				}
				if(dstMode == 1) {
					if(line == 2) {
						f = &MdCpu::Movea<4>;
					} else if(line == 3) {
						f = &MdCpu::Movea<2>;
					}
				} else if(IsValidEa(dstMode, dstReg, MdEaMode::DataAlterable)) {
					switch(line) {
						case 1: f = &MdCpu::Move<1>; break;
						case 2: f = &MdCpu::Move<4>; break;
						case 3: f = &MdCpu::Move<2>; break;
					}
				}
				break;
			}

			case 0x4:
				if((op & 0x01C0) == 0x01C0) {
					if(IsValidEa(mode, reg, MdEaMode::Control)) {
						f = &MdCpu::Lea;
					}
				} else if((op & 0x01C0) == 0x0180) {
					if(IsValidEa(mode, reg, MdEaMode::Data)) {
						f = &MdCpu::Chk;
					}
				} else if(op & 0x0100) {
					//CHK.L (68020+), invalid
				} else {
					switch((op >> 8) & 0x0F) {
						case 0x0:
							if(size == 3) {
								if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
									f = &MdCpu::MoveFromSr;
								}
							} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
								switch(size) {
									case 0: f = &MdCpu::UnaryOp<1, MdUnaryOp::Negx>; break;
									case 1: f = &MdCpu::UnaryOp<2, MdUnaryOp::Negx>; break;
									case 2: f = &MdCpu::UnaryOp<4, MdUnaryOp::Negx>; break;
								}
							}
							break;

						case 0x2:
							if(size != 3 && IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
								switch(size) {
									case 0: f = &MdCpu::UnaryOp<1, MdUnaryOp::Clr>; break;
									case 1: f = &MdCpu::UnaryOp<2, MdUnaryOp::Clr>; break;
									case 2: f = &MdCpu::UnaryOp<4, MdUnaryOp::Clr>; break;
								}
							}
							break;

						case 0x4:
							if(size == 3) {
								if(IsValidEa(mode, reg, MdEaMode::Data)) {
									f = &MdCpu::MoveToSr<false>;
								}
							} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
								switch(size) {
									case 0: f = &MdCpu::UnaryOp<1, MdUnaryOp::Neg>; break;
									case 1: f = &MdCpu::UnaryOp<2, MdUnaryOp::Neg>; break;
									case 2: f = &MdCpu::UnaryOp<4, MdUnaryOp::Neg>; break;
								}
							}
							break;

						case 0x6:
							if(size == 3) {
								if(IsValidEa(mode, reg, MdEaMode::Data)) {
									f = &MdCpu::MoveToSr<true>;
								}
							} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
								switch(size) {
									case 0: f = &MdCpu::UnaryOp<1, MdUnaryOp::Not>; break;
									case 1: f = &MdCpu::UnaryOp<2, MdUnaryOp::Not>; break;
									case 2: f = &MdCpu::UnaryOp<4, MdUnaryOp::Not>; break;
								}
							}
							break;

						case 0x8:
							switch(size) {
								case 0:
									if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
										f = &MdCpu::Nbcd;
									}
									break;

								case 1:
									if(mode == 0) {
										f = &MdCpu::Swap;
									} else if(IsValidEa(mode, reg, MdEaMode::Control)) {
										f = &MdCpu::Pea;
									}
									break;

								case 2:
								case 3:
									if(mode == 0) {
										f = &MdCpu::Ext;
									} else if(IsValidEa(mode, reg, MdEaMode::ControlAlterable | MdEaMode::PreDec)) {
										f = size == 2 ? &MdCpu::Movem<2, true> : &MdCpu::Movem<4, true>;
									}
									break;
							}
							break;

						case 0xA:
							if(op == 0x4AFC) {
								f = &MdCpu::Illegal;
							} else if(size == 3) {
								if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
									f = &MdCpu::Tas;
								}
							} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
								switch(size) {
									case 0: f = &MdCpu::Tst<1>; break;
									case 1: f = &MdCpu::Tst<2>; break;
									case 2: f = &MdCpu::Tst<4>; break;
								}
							}
							break;

						case 0xC:
							if(size >= 2 && IsValidEa(mode, reg, MdEaMode::Control | MdEaMode::PostInc)) {
								f = size == 2 ? &MdCpu::Movem<2, false> : &MdCpu::Movem<4, false>;
							}
							break;

						case 0xE:
							if((op & 0xFFF0) == 0x4E40) {
								f = &MdCpu::Trap;
							} else if((op & 0xFFF8) == 0x4E50) {
								f = &MdCpu::Link;
							} else if((op & 0xFFF8) == 0x4E58) {
								f = &MdCpu::Unlk;
							} else if((op & 0xFFF8) == 0x4E60) {
								f = &MdCpu::MoveUsp<true>;
							} else if((op & 0xFFF8) == 0x4E68) {
								f = &MdCpu::MoveUsp<false>;
							} else if(op == 0x4E70) {
								f = &MdCpu::ResetInstruction;
							} else if(op == 0x4E71) {
								f = &MdCpu::Nop;
							} else if(op == 0x4E72) {
								f = &MdCpu::Stop;
							} else if(op == 0x4E73) {
								f = &MdCpu::Rte;
							} else if(op == 0x4E75) {
								f = &MdCpu::Rts;
							} else if(op == 0x4E76) {
								f = &MdCpu::Trapv;
							} else if(op == 0x4E77) {
								f = &MdCpu::Rtr;
							} else if(size == 2 && IsValidEa(mode, reg, MdEaMode::Control)) {
								f = &MdCpu::Jsr;
							} else if(size == 3 && IsValidEa(mode, reg, MdEaMode::Control)) {
								f = &MdCpu::Jmp;
							}
							break;
					}
				}
				break;

			case 0x5:
				if(size == 3) {
					if(mode == 1) {
						f = &MdCpu::Dbcc;
					} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
						f = &MdCpu::Scc;
					}
				} else if(IsValidEa(mode, reg, size == 0 ? MdEaMode::DataAlterable : MdEaMode::Alterable)) {
					bool isSub = op & 0x100;
					switch(size) {
						case 0: f = isSub ? &MdCpu::ArithQuick<1, true> : &MdCpu::ArithQuick<1, false>; break;
						case 1: f = isSub ? &MdCpu::ArithQuick<2, true> : &MdCpu::ArithQuick<2, false>; break;
						case 2: f = isSub ? &MdCpu::ArithQuick<4, true> : &MdCpu::ArithQuick<4, false>; break;
					}
				}
				break;

			case 0x6:
				f = ((op >> 8) & 0x0F) == 1 ? &MdCpu::Bsr : &MdCpu::Bcc;
				break;

			case 0x7:
				if(!(op & 0x100)) {
					f = &MdCpu::Moveq;
				}
				break;

			case 0x8:
			case 0xC: {
				bool isAnd = (op >> 12) == 0xC;
				if(opMode == 3) {
					if(IsValidEa(mode, reg, MdEaMode::Data)) {
						f = isAnd ? &MdCpu::Mulu : &MdCpu::Divu;
					}
				} else if(opMode == 7) {
					if(IsValidEa(mode, reg, MdEaMode::Data)) {
						f = isAnd ? &MdCpu::Muls : &MdCpu::Divs;
					}
				} else if(opMode == 4 && mode <= 1) {
					f = isAnd ? &MdCpu::Bcd<false> : &MdCpu::Bcd<true>;
				} else if(isAnd && opMode == 5 && mode <= 1) {
					f = &MdCpu::Exg;
				} else if(isAnd && opMode == 6 && mode == 1) {
					f = &MdCpu::Exg;
				} else if((opMode == 5 || opMode == 6) && mode <= 1) {
					//Invalid
				} else if(opMode < 3) {
					if(IsValidEa(mode, reg, MdEaMode::Data)) {
						switch(size) {
							case 0: f = isAnd ? &MdCpu::ArithEaToDn<1, MdArithOp::And> : &MdCpu::ArithEaToDn<1, MdArithOp::Or>; break;
							case 1: f = isAnd ? &MdCpu::ArithEaToDn<2, MdArithOp::And> : &MdCpu::ArithEaToDn<2, MdArithOp::Or>; break;
							case 2: f = isAnd ? &MdCpu::ArithEaToDn<4, MdArithOp::And> : &MdCpu::ArithEaToDn<4, MdArithOp::Or>; break;
						}
					}
				} else if(IsValidEa(mode, reg, MdEaMode::MemoryAlterable)) {
					switch(size) {
						case 0: f = isAnd ? &MdCpu::ArithDnToEa<1, MdArithOp::And> : &MdCpu::ArithDnToEa<1, MdArithOp::Or>; break;
						case 1: f = isAnd ? &MdCpu::ArithDnToEa<2, MdArithOp::And> : &MdCpu::ArithDnToEa<2, MdArithOp::Or>; break;
						case 2: f = isAnd ? &MdCpu::ArithDnToEa<4, MdArithOp::And> : &MdCpu::ArithDnToEa<4, MdArithOp::Or>; break;
					}
				}
				break;
			}

			case 0x9:
			case 0xD: {
				bool isSub = (op >> 12) == 0x9;
				if(opMode == 3 || opMode == 7) {
					if(IsValidEa(mode, reg, MdEaMode::All)) {
						if(opMode == 3) {
							f = isSub ? &MdCpu::ArithAddress<2, true> : &MdCpu::ArithAddress<2, false>;
						} else {
							f = isSub ? &MdCpu::ArithAddress<4, true> : &MdCpu::ArithAddress<4, false>;
						}
					}
				} else if(opMode < 3) {
					if(IsValidEa(mode, reg, size == 0 ? MdEaMode::Data : MdEaMode::All)) {
						switch(size) {
							case 0: f = isSub ? &MdCpu::ArithEaToDn<1, MdArithOp::Sub> : &MdCpu::ArithEaToDn<1, MdArithOp::Add>; break;
							case 1: f = isSub ? &MdCpu::ArithEaToDn<2, MdArithOp::Sub> : &MdCpu::ArithEaToDn<2, MdArithOp::Add>; break;
							case 2: f = isSub ? &MdCpu::ArithEaToDn<4, MdArithOp::Sub> : &MdCpu::ArithEaToDn<4, MdArithOp::Add>; break;
						}
					}
				} else if(mode <= 1) {
					switch(size) {
						case 0: f = isSub ? &MdCpu::ArithExtend<1, true> : &MdCpu::ArithExtend<1, false>; break;
						case 1: f = isSub ? &MdCpu::ArithExtend<2, true> : &MdCpu::ArithExtend<2, false>; break;
						case 2: f = isSub ? &MdCpu::ArithExtend<4, true> : &MdCpu::ArithExtend<4, false>; break;
					}
				} else if(IsValidEa(mode, reg, MdEaMode::MemoryAlterable)) {
					switch(size) {
						case 0: f = isSub ? &MdCpu::ArithDnToEa<1, MdArithOp::Sub> : &MdCpu::ArithDnToEa<1, MdArithOp::Add>; break;
						case 1: f = isSub ? &MdCpu::ArithDnToEa<2, MdArithOp::Sub> : &MdCpu::ArithDnToEa<2, MdArithOp::Add>; break;
						case 2: f = isSub ? &MdCpu::ArithDnToEa<4, MdArithOp::Sub> : &MdCpu::ArithDnToEa<4, MdArithOp::Add>; break;
					}
				}
				break;
			}

			case 0xA:
				f = &MdCpu::LineA;
				break;

			case 0xB:
				if(opMode == 3 || opMode == 7) {
					if(IsValidEa(mode, reg, MdEaMode::All)) {
						f = opMode == 3 ? &MdCpu::Cmpa<2> : &MdCpu::Cmpa<4>;
					}
				} else if(opMode < 3) {
					if(IsValidEa(mode, reg, size == 0 ? MdEaMode::Data : MdEaMode::All)) {
						switch(size) {
							case 0: f = &MdCpu::Cmp<1>; break;
							case 1: f = &MdCpu::Cmp<2>; break;
							case 2: f = &MdCpu::Cmp<4>; break;
						}
					}
				} else if(mode == 1) {
					switch(size) {
						case 0: f = &MdCpu::Cmpm<1>; break;
						case 1: f = &MdCpu::Cmpm<2>; break;
						case 2: f = &MdCpu::Cmpm<4>; break;
					}
				} else if(IsValidEa(mode, reg, MdEaMode::DataAlterable)) {
					switch(size) {
						case 0: f = &MdCpu::ArithDnToEa<1, MdArithOp::Eor>; break;
						case 1: f = &MdCpu::ArithDnToEa<2, MdArithOp::Eor>; break;
						case 2: f = &MdCpu::ArithDnToEa<4, MdArithOp::Eor>; break;
					}
				}
				break;

			case 0xE:
				if(size == 3) {
					if(!(op & 0x0800) && IsValidEa(mode, reg, MdEaMode::MemoryAlterable)) {
						bool left = op & 0x100;
						switch((op >> 9) & 0x03) {
							case 0: f = left ? &MdCpu::ShiftMemory<MdShiftType::As, true> : &MdCpu::ShiftMemory<MdShiftType::As, false>; break;
							case 1: f = left ? &MdCpu::ShiftMemory<MdShiftType::Ls, true> : &MdCpu::ShiftMemory<MdShiftType::Ls, false>; break;
							case 2: f = left ? &MdCpu::ShiftMemory<MdShiftType::Rox, true> : &MdCpu::ShiftMemory<MdShiftType::Rox, false>; break;
							case 3: f = left ? &MdCpu::ShiftMemory<MdShiftType::Ro, true> : &MdCpu::ShiftMemory<MdShiftType::Ro, false>; break;
						}
					}
				} else {
					bool left = op & 0x100;
					uint8_t type = (op >> 3) & 0x03;
					#define MD_SHIFT_OP(sz) \
						switch(type) { \
							case 0: f = left ? &MdCpu::ShiftRegister<sz, MdShiftType::As, true> : &MdCpu::ShiftRegister<sz, MdShiftType::As, false>; break; \
							case 1: f = left ? &MdCpu::ShiftRegister<sz, MdShiftType::Ls, true> : &MdCpu::ShiftRegister<sz, MdShiftType::Ls, false>; break; \
							case 2: f = left ? &MdCpu::ShiftRegister<sz, MdShiftType::Rox, true> : &MdCpu::ShiftRegister<sz, MdShiftType::Rox, false>; break; \
							case 3: f = left ? &MdCpu::ShiftRegister<sz, MdShiftType::Ro, true> : &MdCpu::ShiftRegister<sz, MdShiftType::Ro, false>; break; \
						}
					switch(size) {
						case 0: MD_SHIFT_OP(1); break;
						case 1: MD_SHIFT_OP(2); break;
						case 2: MD_SHIFT_OP(4); break;
					}
					#undef MD_SHIFT_OP
				}
				break;

			case 0xF:
				f = &MdCpu::LineF;
				break;
		}
	}
}
