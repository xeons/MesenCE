#include "pch.h"
#include "MD/Debugger/MdDisUtils.h"
#include "MD/MdTypes.h"
#include "MD/MdConsole.h"
#include "Debugger/DisassemblyInfo.h"
#include "Debugger/LabelManager.h"
#include "Debugger/MemoryDumper.h"
#include "Shared/MemoryType.h"
#include "Shared/EmuSettings.h"
#include "Utilities/FastString.h"
#include "Utilities/HexUtilities.h"

static constexpr const char* _conditions[16] = { "T", "F", "HI", "LS", "CC", "CS", "NE", "EQ", "VC", "VS", "PL", "MI", "GE", "LT", "GT", "LE" };
static constexpr const char* _sizeSuffix[5] = { "", ".B", ".W", "", ".L" };

//Decodes a single 68000 instruction - used to generate the disassembly text, the instruction size and the effective address
class MdInstructionDecoder
{
private:
	uint16_t _op = 0;
	const uint8_t* _byteCode = nullptr;
	uint8_t _pos = 2;
	uint32_t _pc = 0;

	FastString* _str = nullptr;
	LabelManager* _labelManager = nullptr;

	MdCpuState* _state = nullptr;
	bool _eaFound = false;
	bool _ignoreEa = false;

	uint16_t ReadWord()
	{
		uint16_t value = 0;
		if(_byteCode && _pos + 1 < MdDisUtils::MaxOpSize) {
			value = (_byteCode[_pos] << 8) | _byteCode[_pos + 1];
		}
		_pos += 2;
		return value;
	}

	uint32_t ReadLong()
	{
		uint32_t hi = ReadWord();
		return (hi << 16) | ReadWord();
	}

	void Write(const char* text)
	{
		if(_str) {
			_str->Write(text);
		}
	}

	void Write(char c)
	{
		if(_str) {
			_str->Write(c);
		}
	}

	void WriteHex(uint32_t value, uint8_t size)
	{
		if(_str) {
			switch(size) {
				case 1: _str->WriteAll('$', HexUtilities::ToHex((uint8_t)value)); break;
				case 2: _str->WriteAll('$', HexUtilities::ToHex((uint16_t)value)); break;
				default: _str->WriteAll('$', HexUtilities::ToHex32(value)); break;
			}
		}
	}

	void WriteSigned(int32_t value)
	{
		if(_str) {
			if(value < 0) {
				_str->Write('-');
				value = -value;
			}
			if(value > 0xFF) {
				_str->WriteAll('$', HexUtilities::ToHex((uint16_t)value));
			} else {
				_str->WriteAll('$', HexUtilities::ToHex((uint8_t)value));
			}
		}
	}

	void WriteAddress(uint32_t addr)
	{
		if(_str) {
			addr &= 0xFFFFFF;
			AddressInfo relAddr = { (int32_t)addr, MemoryType::MdMemory };
			string label = _labelManager ? _labelManager->GetLabel(relAddr) : "";
			if(label.empty()) {
				_str->WriteAll('$', HexUtilities::ToHex24(addr));
			} else {
				_str->Write(label, true);
			}
		}
	}

	void WriteReg(bool isAddress, uint8_t reg)
	{
		if(_str) {
			_str->Write(isAddress ? 'A' : 'D');
			_str->Write((char)('0' + reg));
		}
	}

	void WriteOp(const char* name, uint8_t size = 0)
	{
		Write(name);
		Write(_sizeSuffix[size]);
		Write(' ');
	}

	void Separator()
	{
		Write(", ");
	}

	void SetEffectiveAddress(uint32_t addr, uint8_t size, bool showAddress, EffectiveAddressInfo* result)
	{
		if(!_eaFound && !_ignoreEa && result) {
			_eaFound = true;
			*result = EffectiveAddressInfo(addr & 0xFFFFFF, size, showAddress);
		}
	}

	uint32_t GetIndexValue(uint16_t ext)
	{
		if(!_state) {
			return 0;
		}

		uint8_t reg = (ext >> 12) & 0x07;
		uint32_t index = (ext & 0x8000) ? _state->A[reg] : _state->D[reg];
		if(!(ext & 0x800)) {
			index = (uint32_t)(int16_t)index;
		}
		return index + (int8_t)ext;
	}

	void WriteIndex(uint16_t ext, const char* baseReg)
	{
		WriteSigned((int8_t)ext);
		Write('(');
		Write(baseReg);
		Write(',');
		WriteReg((ext & 0x8000) != 0, (ext >> 12) & 0x07);
		Write((ext & 0x800) ? ".L)" : ".W)");
	}

	void Ea(uint8_t mode, uint8_t reg, uint8_t size, EffectiveAddressInfo* result = nullptr)
	{
		mode &= 0x07;
		reg &= 0x07;

		switch(mode) {
			case 0: WriteReg(false, reg); break;
			case 1: WriteReg(true, reg); break;

			case 2:
				Write('('); WriteReg(true, reg); Write(')');
				if(_state) {
					SetEffectiveAddress(_state->A[reg], size, true, result);
				}
				break;

			case 3:
				Write('('); WriteReg(true, reg); Write(")+");
				if(_state) {
					SetEffectiveAddress(_state->A[reg], size, true, result);
				}
				break;

			case 4:
				Write("-("); WriteReg(true, reg); Write(')');
				if(_state) {
					uint8_t decrement = (size == 1 && reg == 7) ? 2 : size;
					SetEffectiveAddress(_state->A[reg] - decrement, size, true, result);
				}
				break;

			case 5: {
				int16_t disp = (int16_t)ReadWord();
				WriteSigned(disp);
				Write('('); WriteReg(true, reg); Write(')');
				if(_state) {
					SetEffectiveAddress(_state->A[reg] + disp, size, true, result);
				}
				break;
			}

			case 6: {
				uint16_t ext = ReadWord();
				char baseReg[3] = { 'A', (char)('0' + reg), 0 };
				WriteIndex(ext, baseReg);
				if(_state) {
					SetEffectiveAddress(_state->A[reg] + GetIndexValue(ext), size, true, result);
				}
				break;
			}

			case 7:
				switch(reg) {
					case 0: {
						uint32_t addr = (uint32_t)(int16_t)ReadWord();
						WriteAddress(addr);
						Write(".W");
						SetEffectiveAddress(addr, size, false, result);
						break;
					}

					case 1: {
						uint32_t addr = ReadLong();
						WriteAddress(addr);
						SetEffectiveAddress(addr, size, false, result);
						break;
					}

					case 2: {
						uint32_t base = _pc + _pos;
						int16_t disp = (int16_t)ReadWord();
						WriteAddress(base + disp);
						Write("(PC)");
						SetEffectiveAddress(base + disp, size, false, result);
						break;
					}

					case 3: {
						uint32_t base = _pc + _pos;
						uint16_t ext = ReadWord();
						WriteIndex(ext, "PC");
						if(_state) {
							SetEffectiveAddress(base + GetIndexValue(ext), size, true, result);
						}
						break;
					}

					case 4:
						Write('#');
						if(size == 4) {
							WriteHex(ReadLong(), 4);
						} else if(size == 2) {
							WriteHex(ReadWord(), 2);
						} else {
							WriteHex(ReadWord() & 0xFF, 1);
						}
						break;

					default:
						Write('?');
						break;
				}
				break;
		}
	}

	void WriteRegList(uint16_t mask, bool reversed)
	{
		if(reversed) {
			uint16_t normal = 0;
			for(int i = 0; i < 16; i++) {
				if(mask & (1 << i)) {
					normal |= 1 << (15 - i);
				}
			}
			mask = normal;
		}

		bool first = true;
		for(int group = 0; group < 2; group++) {
			int i = 0;
			while(i < 8) {
				if(mask & (1 << (group * 8 + i))) {
					int start = i;
					while(i + 1 < 8 && (mask & (1 << (group * 8 + i + 1)))) {
						i++;
					}
					if(!first) {
						Write('/');
					}
					first = false;
					WriteReg(group == 1, start);
					if(i > start) {
						Write('-');
						WriteReg(group == 1, i);
					}
				}
				i++;
			}
		}

		if(first) {
			Write("#0");
		}
	}

	void Invalid()
	{
		Write("DC.W ");
		WriteHex(_op, 2);
	}

	static uint8_t GetSize(uint8_t sizeBits)
	{
		switch(sizeBits & 0x03) {
			case 0: return 1;
			case 1: return 2;
			case 2: return 4;
			default: return 0;
		}
	}

	void DecodeGroup0(EffectiveAddressInfo* ea)
	{
		uint8_t mode = (_op >> 3) & 0x07;
		uint8_t reg = _op & 0x07;

		if((_op & 0x0138) == 0x0108) {
			//MOVEP
			uint8_t dataReg = (_op >> 9) & 0x07;
			uint8_t opMode = (_op >> 6) & 0x07;
			uint8_t size = (opMode & 0x01) ? 4 : 2;
			WriteOp("MOVEP", size);
			int16_t disp = (int16_t)ReadWord();
			if(opMode & 0x02) {
				WriteReg(false, dataReg);
				Separator();
				WriteSigned(disp); Write('('); WriteReg(true, reg); Write(')');
			} else {
				WriteSigned(disp); Write('('); WriteReg(true, reg); Write(')');
				Separator();
				WriteReg(false, dataReg);
			}
			if(_state) {
				SetEffectiveAddress(_state->A[reg] + disp, 1, true, ea);
			}
			return;
		}

		static constexpr const char* bitOps[4] = { "BTST", "BCHG", "BCLR", "BSET" };
		if(_op & 0x100) {
			//Dynamic bit operations
			WriteOp(bitOps[(_op >> 6) & 0x03]);
			WriteReg(false, (_op >> 9) & 0x07);
			Separator();
			Ea(mode, reg, 1, ea);
			return;
		}

		uint8_t kind = (_op >> 9) & 0x07;
		if(kind == 4) {
			//Static bit operations
			WriteOp(bitOps[(_op >> 6) & 0x03]);
			Write('#');
			WriteHex(ReadWord() & 0xFF, 1);
			Separator();
			Ea(mode, reg, 1, ea);
			return;
		}

		static constexpr const char* immOps[8] = { "ORI", "ANDI", "SUBI", "ADDI", "", "EORI", "CMPI", "" };
		uint8_t sizeBits = (_op >> 6) & 0x03;
		if(kind == 7 || sizeBits == 3) {
			Invalid();
			return;
		}

		if((_op & 0x3F) == 0x3C && (kind == 0 || kind == 1 || kind == 5)) {
			//ORI/ANDI/EORI to CCR/SR
			if(sizeBits > 1) {
				Invalid();
				return;
			}
			WriteOp(immOps[kind]);
			Write('#');
			if(sizeBits == 0) {
				WriteHex(ReadWord() & 0xFF, 1);
				Write(", CCR");
			} else {
				WriteHex(ReadWord(), 2);
				Write(", SR");
			}
			return;
		}

		uint8_t size = GetSize(sizeBits);
		WriteOp(immOps[kind], size);
		Ea(7, 4, size);
		Separator();
		Ea(mode, reg, size, ea);
	}

	void DecodeMove(uint8_t size, EffectiveAddressInfo* ea)
	{
		uint8_t dstMode = (_op >> 6) & 0x07;
		uint8_t dstReg = (_op >> 9) & 0x07;
		if(dstMode == 1) {
			if(size == 1) {
				Invalid();
				return;
			}
			WriteOp("MOVEA", size);
		} else {
			WriteOp("MOVE", size);
		}
		Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
		Separator();
		Ea(dstMode, dstReg, size, ea);
	}

	void DecodeGroup4(EffectiveAddressInfo* ea)
	{
		uint8_t mode = (_op >> 3) & 0x07;
		uint8_t reg = _op & 0x07;
		uint8_t sizeBits = (_op >> 6) & 0x03;

		if((_op & 0x1C0) == 0x1C0) {
			_ignoreEa = true;
			WriteOp("LEA");
			Ea(mode, reg, 4);
			Separator();
			WriteReg(true, (_op >> 9) & 0x07);
			return;
		} else if((_op & 0x1C0) == 0x180) {
			WriteOp("CHK", 2);
			Ea(mode, reg, 2, ea);
			Separator();
			WriteReg(false, (_op >> 9) & 0x07);
			return;
		}

		switch((_op >> 8) & 0x0F) {
			case 0x0:
				if(sizeBits == 3) {
					WriteOp("MOVE", 2);
					Write("SR, ");
					Ea(mode, reg, 2, ea);
				} else {
					WriteOp("NEGX", GetSize(sizeBits));
					Ea(mode, reg, GetSize(sizeBits), ea);
				}
				break;

			case 0x2:
				if(sizeBits == 3) {
					Invalid();
				} else {
					WriteOp("CLR", GetSize(sizeBits));
					Ea(mode, reg, GetSize(sizeBits), ea);
				}
				break;

			case 0x4:
				if(sizeBits == 3) {
					WriteOp("MOVE", 2);
					Ea(mode, reg, 2, ea);
					Write(", CCR");
				} else {
					WriteOp("NEG", GetSize(sizeBits));
					Ea(mode, reg, GetSize(sizeBits), ea);
				}
				break;

			case 0x6:
				if(sizeBits == 3) {
					WriteOp("MOVE", 2);
					Ea(mode, reg, 2, ea);
					Write(", SR");
				} else {
					WriteOp("NOT", GetSize(sizeBits));
					Ea(mode, reg, GetSize(sizeBits), ea);
				}
				break;

			case 0x8:
				switch(sizeBits) {
					case 0:
						WriteOp("NBCD", 1);
						Ea(mode, reg, 1, ea);
						break;

					case 1:
						if(mode == 0) {
							WriteOp("SWAP");
							WriteReg(false, reg);
						} else {
							_ignoreEa = true;
							WriteOp("PEA");
							Ea(mode, reg, 4);
						}
						break;

					default:
						if(mode == 0) {
							WriteOp("EXT", sizeBits == 2 ? 2 : 4);
							WriteReg(false, reg);
						} else {
							_ignoreEa = true;
							uint16_t mask = ReadWord();
							WriteOp("MOVEM", sizeBits == 2 ? 2 : 4);
							WriteRegList(mask, mode == 4);
							Separator();
							Ea(mode, reg, sizeBits == 2 ? 2 : 4);
						}
						break;
				}
				break;

			case 0xA:
				if(_op == 0x4AFC) {
					Write("ILLEGAL");
				} else if(sizeBits == 3) {
					WriteOp("TAS", 1);
					Ea(mode, reg, 1, ea);
				} else {
					WriteOp("TST", GetSize(sizeBits));
					Ea(mode, reg, GetSize(sizeBits), ea);
				}
				break;

			case 0xC:
				if(sizeBits >= 2) {
					_ignoreEa = true;
					uint16_t mask = ReadWord();
					WriteOp("MOVEM", sizeBits == 2 ? 2 : 4);
					Ea(mode, reg, sizeBits == 2 ? 2 : 4);
					Separator();
					WriteRegList(mask, false);
				} else {
					Invalid();
				}
				break;

			case 0xE:
				if((_op & 0xFFF0) == 0x4E40) {
					WriteOp("TRAP");
					Write('#');
					WriteHex(_op & 0x0F, 1);
				} else if((_op & 0xFFF8) == 0x4E50) {
					WriteOp("LINK");
					WriteReg(true, reg);
					Write(", #");
					WriteSigned((int16_t)ReadWord());
				} else if((_op & 0xFFF8) == 0x4E58) {
					WriteOp("UNLK");
					WriteReg(true, reg);
				} else if((_op & 0xFFF8) == 0x4E60) {
					WriteOp("MOVE");
					WriteReg(true, reg);
					Write(", USP");
				} else if((_op & 0xFFF8) == 0x4E68) {
					WriteOp("MOVE");
					Write("USP, ");
					WriteReg(true, reg);
				} else if((_op & 0xFFC0) == 0x4E80) {
					_ignoreEa = true;
					WriteOp("JSR");
					Ea(mode, reg, 4);
				} else if((_op & 0xFFC0) == 0x4EC0) {
					_ignoreEa = true;
					WriteOp("JMP");
					Ea(mode, reg, 4);
				} else {
					switch(_op) {
						case 0x4E70: Write("RESET"); break;
						case 0x4E71: Write("NOP"); break;
						case 0x4E72: WriteOp("STOP"); Write('#'); WriteHex(ReadWord(), 2); break;
						case 0x4E73: Write("RTE"); break;
						case 0x4E75: Write("RTS"); break;
						case 0x4E76: Write("TRAPV"); break;
						case 0x4E77: Write("RTR"); break;
						default: Invalid(); break;
					}
				}
				break;

			default:
				Invalid();
				break;
		}
	}

	void DecodeGroup5(EffectiveAddressInfo* ea)
	{
		uint8_t mode = (_op >> 3) & 0x07;
		uint8_t reg = _op & 0x07;
		uint8_t sizeBits = (_op >> 6) & 0x03;
		uint8_t cond = (_op >> 8) & 0x0F;

		if(sizeBits == 3) {
			if(mode == 1) {
				Write("DB");
				Write(cond == 1 ? "RA" : _conditions[cond]);
				Write(' ');
				WriteReg(false, reg);
				Separator();
				uint32_t base = _pc + 2;
				WriteAddress(base + (int16_t)ReadWord());
			} else {
				Write('S');
				Write(_conditions[cond]);
				Write(' ');
				Ea(mode, reg, 1, ea);
			}
			return;
		}

		uint8_t size = GetSize(sizeBits);
		uint8_t data = (_op >> 9) & 0x07;
		WriteOp((_op & 0x100) ? "SUBQ" : "ADDQ", size);
		Write('#');
		WriteHex(data == 0 ? 8 : data, 1);
		Separator();
		Ea(mode, reg, size, ea);
	}

	void DecodeBranch()
	{
		uint8_t cond = (_op >> 8) & 0x0F;
		uint8_t disp8 = _op & 0xFF;
		uint32_t base = _pc + 2;

		if(cond == 0) {
			Write("BRA");
		} else if(cond == 1) {
			Write("BSR");
		} else {
			Write('B');
			Write(_conditions[cond]);
		}

		if(disp8 == 0) {
			Write(".W ");
			WriteAddress(base + (int16_t)ReadWord());
		} else {
			Write(".S ");
			WriteAddress(base + (int8_t)disp8);
		}
	}

	void DecodeOrAnd(const char* name, EffectiveAddressInfo* ea)
	{
		uint8_t opMode = (_op >> 6) & 0x07;
		uint8_t dataReg = (_op >> 9) & 0x07;
		uint8_t size = GetSize(opMode);
		WriteOp(name, size);
		if(opMode & 0x04) {
			WriteReg(false, dataReg);
			Separator();
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
		} else {
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
			Separator();
			WriteReg(false, dataReg);
		}
	}

	void DecodeExtended(const char* name, uint8_t size)
	{
		uint8_t rx = (_op >> 9) & 0x07;
		uint8_t ry = _op & 0x07;
		WriteOp(name, size);
		if(_op & 0x08) {
			Write("-("); WriteReg(true, ry); Write("), -("); WriteReg(true, rx); Write(')');
		} else {
			WriteReg(false, ry);
			Separator();
			WriteReg(false, rx);
		}
	}

	void DecodeGroup8(EffectiveAddressInfo* ea)
	{
		uint8_t opMode = (_op >> 6) & 0x07;
		if(opMode == 3 || opMode == 7) {
			WriteOp(opMode == 3 ? "DIVU" : "DIVS", 2);
			Ea((_op >> 3) & 0x07, _op & 0x07, 2, ea);
			Separator();
			WriteReg(false, (_op >> 9) & 0x07);
		} else if((_op & 0x1F0) == 0x100) {
			DecodeExtended("SBCD", 1);
		} else {
			DecodeOrAnd("OR", ea);
		}
	}

	void DecodeAddSub(bool isAdd, EffectiveAddressInfo* ea)
	{
		uint8_t opMode = (_op >> 6) & 0x07;
		if(opMode == 3 || opMode == 7) {
			uint8_t size = opMode == 3 ? 2 : 4;
			WriteOp(isAdd ? "ADDA" : "SUBA", size);
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
			Separator();
			WriteReg(true, (_op >> 9) & 0x07);
		} else if((_op & 0x130) == 0x100) {
			DecodeExtended(isAdd ? "ADDX" : "SUBX", GetSize(opMode));
		} else {
			DecodeOrAnd(isAdd ? "ADD" : "SUB", ea);
		}
	}

	void DecodeGroupB(EffectiveAddressInfo* ea)
	{
		uint8_t opMode = (_op >> 6) & 0x07;
		uint8_t dataReg = (_op >> 9) & 0x07;
		if(opMode == 3 || opMode == 7) {
			uint8_t size = opMode == 3 ? 2 : 4;
			WriteOp("CMPA", size);
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
			Separator();
			WriteReg(true, dataReg);
		} else if(opMode < 3) {
			uint8_t size = GetSize(opMode);
			WriteOp("CMP", size);
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
			Separator();
			WriteReg(false, dataReg);
		} else if(((_op >> 3) & 0x07) == 1) {
			WriteOp("CMPM", GetSize(opMode));
			Write('('); WriteReg(true, _op & 0x07); Write(")+, (");
			WriteReg(true, dataReg); Write(")+");
		} else {
			uint8_t size = GetSize(opMode);
			WriteOp("EOR", size);
			WriteReg(false, dataReg);
			Separator();
			Ea((_op >> 3) & 0x07, _op & 0x07, size, ea);
		}
	}

	void DecodeGroupC(EffectiveAddressInfo* ea)
	{
		uint8_t opMode = (_op >> 6) & 0x07;
		uint8_t rx = (_op >> 9) & 0x07;
		uint8_t ry = _op & 0x07;
		if(opMode == 3 || opMode == 7) {
			WriteOp(opMode == 3 ? "MULU" : "MULS", 2);
			Ea((_op >> 3) & 0x07, ry, 2, ea);
			Separator();
			WriteReg(false, rx);
		} else if((_op & 0x1F8) == 0x140) {
			WriteOp("EXG");
			WriteReg(false, rx); Separator(); WriteReg(false, ry);
		} else if((_op & 0x1F8) == 0x148) {
			WriteOp("EXG");
			WriteReg(true, rx); Separator(); WriteReg(true, ry);
		} else if((_op & 0x1F8) == 0x188) {
			WriteOp("EXG");
			WriteReg(false, rx); Separator(); WriteReg(true, ry);
		} else if((_op & 0x1F0) == 0x100) {
			DecodeExtended("ABCD", 1);
		} else {
			DecodeOrAnd("AND", ea);
		}
	}

	void DecodeShift(EffectiveAddressInfo* ea)
	{
		static constexpr const char* shiftOps[4] = { "AS", "LS", "ROX", "RO" };
		uint8_t sizeBits = (_op >> 6) & 0x03;
		bool left = (_op & 0x100) != 0;

		if(sizeBits == 3) {
			if(_op & 0x800) {
				Invalid();
				return;
			}
			Write(shiftOps[(_op >> 9) & 0x03]);
			Write(left ? "L" : "R");
			Write(".W ");
			Ea((_op >> 3) & 0x07, _op & 0x07, 2, ea);
			return;
		}

		uint8_t size = GetSize(sizeBits);
		uint8_t count = (_op >> 9) & 0x07;
		Write(shiftOps[(_op >> 3) & 0x03]);
		Write(left ? "L" : "R");
		Write(_sizeSuffix[size]);
		Write(' ');
		if(_op & 0x20) {
			WriteReg(false, count);
		} else {
			Write('#');
			WriteHex(count == 0 ? 8 : count, 1);
		}
		Separator();
		WriteReg(false, _op & 0x07);
	}

public:
	MdInstructionDecoder(uint16_t op, const uint8_t* byteCode, uint32_t pc, FastString* str, LabelManager* labelManager, MdCpuState* state)
	{
		_op = op;
		_byteCode = byteCode;
		_pc = pc;
		_str = str;
		_labelManager = labelManager;
		_state = state;
	}

	uint8_t Decode(EffectiveAddressInfo* ea = nullptr)
	{
		switch(_op >> 12) {
			case 0x0: DecodeGroup0(ea); break;
			case 0x1: DecodeMove(1, ea); break;
			case 0x2: DecodeMove(4, ea); break;
			case 0x3: DecodeMove(2, ea); break;
			case 0x4: DecodeGroup4(ea); break;
			case 0x5: DecodeGroup5(ea); break;
			case 0x6: DecodeBranch(); break;

			case 0x7:
				if(_op & 0x100) {
					Invalid();
				} else {
					WriteOp("MOVEQ");
					Write('#');
					WriteSigned((int8_t)_op);
					Separator();
					WriteReg(false, (_op >> 9) & 0x07);
				}
				break;

			case 0x8: DecodeGroup8(ea); break;
			case 0x9: DecodeAddSub(false, ea); break;
			case 0xA: Write("LINEA "); WriteHex(_op, 2); break;
			case 0xB: DecodeGroupB(ea); break;
			case 0xC: DecodeGroupC(ea); break;
			case 0xD: DecodeAddSub(true, ea); break;
			case 0xE: DecodeShift(ea); break;
			case 0xF: Write("LINEF "); WriteHex(_op, 2); break;
		}

		return std::min<uint8_t>(_pos, MdDisUtils::MaxOpSize);
	}
};

void MdDisUtils::GetDisassembly(DisassemblyInfo& info, string& out, uint32_t memoryAddr, LabelManager* labelManager, EmuSettings* settings)
{
	FastString str(settings->GetDebugConfig().UseLowerCaseDisassembly);
	MdInstructionDecoder decoder(GetOpCode(info), info.GetByteCode(), memoryAddr, &str, labelManager, nullptr);
	decoder.Decode();
	out += str.ToString();
}

EffectiveAddressInfo MdDisUtils::GetEffectiveAddress(DisassemblyInfo& info, MdConsole* console, MdCpuState& state)
{
	uint16_t opCode = GetOpCode(info);
	if(IsJumpToSub(opCode) || IsReturnInstruction(opCode) || IsUnconditionalJump(opCode) || IsConditionalJump(opCode)) {
		return {};
	}

	EffectiveAddressInfo result;
	MdInstructionDecoder decoder(opCode, info.GetByteCode(), state.PC, nullptr, nullptr, &state);
	decoder.Decode(&result);
	return result;
}

uint8_t MdDisUtils::GetOpSize(uint16_t opCode)
{
	MdInstructionDecoder decoder(opCode, nullptr, 0, nullptr, nullptr, nullptr);
	return decoder.Decode();
}

uint8_t MdDisUtils::GetOpSize(uint32_t cpuAddress, MemoryType memType, MemoryDumper* memoryDumper)
{
	uint16_t opCode = (memoryDumper->GetMemoryValue(memType, cpuAddress) << 8) | memoryDumper->GetMemoryValue(memType, cpuAddress + 1);
	return GetOpSize(opCode);
}

uint16_t MdDisUtils::GetOpCode(DisassemblyInfo& info)
{
	uint8_t* byteCode = info.GetByteCode();
	return (byteCode[0] << 8) | byteCode[1];
}

bool MdDisUtils::IsJumpToSub(uint16_t opCode)
{
	return (
		(opCode & 0xFFC0) == 0x4E80 || //JSR
		(opCode & 0xFF00) == 0x6100 || //BSR
		(opCode & 0xFFF0) == 0x4E40 //TRAP
	);
}

bool MdDisUtils::IsReturnInstruction(uint16_t opCode)
{
	return opCode == 0x4E73 || opCode == 0x4E75 || opCode == 0x4E77; //RTE, RTS, RTR
}

bool MdDisUtils::IsUnconditionalJump(uint16_t opCode)
{
	return (
		(opCode & 0xFF00) == 0x6000 || //BRA
		(opCode & 0xFFC0) == 0x4EC0 || //JMP
		IsReturnInstruction(opCode)
	);
}

bool MdDisUtils::IsConditionalJump(uint16_t opCode)
{
	return (
		((opCode & 0xF000) == 0x6000 && (opCode & 0x0E00) != 0) || //Bcc (excluding BRA/BSR)
		(opCode & 0xF0F8) == 0x50C8 //DBcc
	);
}

CdlFlags::CdlFlags MdDisUtils::GetOpFlags(uint16_t opCode, uint32_t pc, uint32_t prevPc, uint8_t opSize)
{
	if(pc == prevPc + opSize) {
		return CdlFlags::None;
	}

	if(IsJumpToSub(opCode)) {
		return CdlFlags::SubEntryPoint;
	} else if(IsConditionalJump(opCode) || (opCode & 0xFF00) == 0x6000 || (opCode & 0xFFC0) == 0x4EC0) {
		return CdlFlags::JumpTarget;
	}
	return CdlFlags::None;
}
