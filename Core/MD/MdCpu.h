#if (defined(DUMMYCPU) && !defined(__DUMMYMdCpu__H)) || (!defined(DUMMYCPU) && !defined(__MdCpu__H))
	#ifdef DUMMYCPU
		#define __DUMMYMdCpu__H
	#else
		#define __MdCpu__H
	#endif

	#include "pch.h"
	#include "MD/MdTypes.h"
	#include "Shared/MemoryOperationType.h"
	#include "Shared/MemoryType.h"
	#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdMemoryManager;

//Motorola 68000
class MdCpu final : public ISerializable
{
private:
	typedef void (MdCpu::*OpFunc)();

	struct AddressErrorException
	{
	};

	static OpFunc _opTable[0x10000];
	static bool _opTableInitialized;

	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdMemoryManager* _memoryManager = nullptr;

	MdCpuState _state = {};

	uint16_t _opCode = 0;
	uint32_t _eaAddr = 0;
	bool _inException = false;
	bool _skipTrace = false;

	//Address error details
	uint32_t _faultAddress = 0;
	uint16_t _faultInfo = 0;
	uint32_t _faultPc = 0;

	static void InitOpTable();

	//Bus access
	void Idle(uint32_t cycles);
	uint8_t BusReadByte(uint32_t addr, MemoryOperationType type);
	uint16_t BusReadWord(uint32_t addr, MemoryOperationType type);
	void BusWriteByte(uint32_t addr, uint8_t value);
	void BusWriteWord(uint32_t addr, uint16_t value);

	[[noreturn]] void AddressError(uint32_t addr, bool isRead, bool isProgram);

	uint8_t ReadByte(uint32_t addr);
	uint16_t ReadWord(uint32_t addr);
	uint32_t ReadLong(uint32_t addr);
	void WriteByte(uint32_t addr, uint8_t value);
	void WriteWord(uint32_t addr, uint16_t value);
	void WriteLong(uint32_t addr, uint32_t value);
	void WriteLongReversed(uint32_t addr, uint32_t value);

	template<int size> uint32_t Read(uint32_t addr);
	template<int size> void Write(uint32_t addr, uint32_t value);
	template<int size> void WriteReversed(uint32_t addr, uint32_t value);

	//Prefetch queue
	uint16_t ReadExt();
	uint16_t ReadProgramWord(uint32_t addr);
	void Prefetch();
	void FillQueue(uint32_t addr, uint8_t idleCycles = 0);

	//Status register
	void SetSR(uint16_t value);
	void SetCCR(uint8_t value) { _state.SR = (_state.SR & 0xFF00) | (value & 0x1F); }
	bool IsSupervisor() { return (_state.SR & MdCpuFlags::Supervisor) != 0; }
	__forceinline void SetFlag(uint16_t flag, bool set) { _state.SR = set ? (_state.SR | flag) : (_state.SR & ~flag); }
	__forceinline bool CheckFlag(uint16_t flag) { return (_state.SR & flag) != 0; }
	bool CheckCondition(uint8_t cond);
	uint8_t GetIrqMask() { return (_state.SR >> 8) & 0x07; }

	//Exceptions
	void ProcessException(uint8_t vector, uint32_t pcToPush, uint8_t preIdleCycles);
	void ProcessAddressError();
	void ProcessInterrupt(uint8_t level);
	void ProcessTrace();
	void PrivilegeViolation();
	bool CheckPrivilege();
	void UpdateIrqLevel();

	//Effective address
	uint32_t GetIndexedAddress(uint32_t base);
	template<int size, bool isMoveDst = false> uint32_t GetEaAddress(uint8_t mode, uint8_t reg);
	template<int size> uint32_t ReadEa(uint8_t mode, uint8_t reg);
	template<int size> uint32_t ReadImmediate();

	template<int size> static constexpr uint32_t SizeMask() { return size == 1 ? 0xFF : (size == 2 ? 0xFFFF : 0xFFFFFFFF); }
	template<int size> static constexpr uint32_t SignBit() { return size == 1 ? 0x80 : (size == 2 ? 0x8000 : 0x80000000); }
	template<int size> static uint32_t SignExtend(uint32_t value);
	template<int size> void SetDataReg(uint8_t reg, uint32_t value);
	template<int size> void SetLogicalFlags(uint32_t result);

	template<int size> uint32_t Add(uint32_t src, uint32_t dst, bool withExtend);
	template<int size> uint32_t Sub(uint32_t src, uint32_t dst, bool withExtend, bool setExtend = true);

	//Instructions
	template<int size, int op> void ArithEaToDn();
	template<int size, int op> void ArithDnToEa();
	template<int size, int op> void ArithImmediate();
	template<int size, bool isSub> void ArithQuick();
	template<int size, bool isSub> void ArithAddress();
	template<int size> void Cmp();
	template<int size> void Cmpa();
	template<int size> void Cmpm();
	template<int size, bool isSub> void ArithExtend();
	template<int size> void Move();
	template<int size> void Movea();
	void Moveq();

	template<int size, int op> void UnaryOp();
	template<int size> void Tst();
	void Tas();
	void Scc();
	void Dbcc();
	void Bcc();
	void Bsr();

	uint8_t AddBcd(uint8_t src, uint8_t dst);
	uint8_t SubBcd(uint8_t src, uint8_t dst);
	void SetBcdFlags(uint32_t result, bool carry, bool overflow);
	template<bool isSub> void Bcd();
	void Nbcd();

	template<int op, bool isImmediate> void BitOp();
	template<int size, bool toMemory> void Movep();
	template<int size, bool toMemory> void Movem();

	void Mulu();
	void Muls();
	void Divu();
	void Divs();

	template<int size, int type, bool left> void ShiftRegister();
	template<int type, bool left> void ShiftMemory();

	void Swap();
	void Ext();
	void Exg();
	void Lea();
	void Pea();
	void Link();
	void Unlk();
	void Jmp();
	void Jsr();
	void Rts();
	void Rte();
	void Rtr();
	void Chk();
	void Trap();
	void Trapv();
	void MoveFromSr();
	template<bool toSr> void MoveToSr();
	template<int op, bool toSr> void LogicImmediateSr();
	template<bool toUsp> void MoveUsp();
	void Nop();
	void ResetInstruction();
	void Stop();
	void Illegal();
	void LineA();
	void LineF();

	static bool IsValidEa(uint8_t mode, uint8_t reg, uint16_t allowedModes);

public:
	void Init(Emulator* emu, MdConsole* console, MdMemoryManager* memoryManager);

	//Hardware reset - loads SSP and PC from the vector table
	void Reset();

	//Executes a single instruction (or exception processing, or idles while stopped/halted)
	void Exec();

	void SetIrqSource(MdIrqSource source);
	void ClearIrqSource(MdIrqSource source);

	MdCpuState& GetState();
	uint64_t GetCycleCount() { return _state.CycleCount; }

	void Serialize(Serializer& s) override;

	#ifdef DUMMYCPU
private:
	uint32_t _memOpCounter = 0;
	MemoryOperationInfo _memOperations[32] = {};

public:
	void SetDummyState(MdCpuState& state);
	uint32_t GetOperationCount();
	void LogMemoryOperation(uint32_t addr, uint16_t value, MemoryOperationType type, MemoryType memType);
	MemoryOperationInfo GetOperationInfo(uint32_t index);
	#endif
};
#endif
