#pragma once
#include "pch.h"
#include "Shared/BaseState.h"
#include "Shared/MemoryType.h"
#include "SMS/SmsTypes.h"

class MdConstants
{
public:
	//Master clock (MCLK) rates
	static constexpr uint32_t NtscMasterClockRate = 53693175;
	static constexpr uint32_t PalMasterClockRate = 53203424;

	//Clock dividers, relative to MCLK
	static constexpr uint32_t CpuDivider = 7;
	static constexpr uint32_t Z80Divider = 15;
	static constexpr uint32_t PsgDivider = MdConstants::Z80Divider * 16;
	static constexpr uint32_t FmSampleDivider = MdConstants::CpuDivider * 144;

	//Every scanline takes 3420 MCLK, in both H32 and H40 modes
	static constexpr uint32_t ClocksPerScanline = 3420;
	static constexpr uint32_t NtscScanlineCount = 262;
	static constexpr uint32_t PalScanlineCount = 313;

	//Output frame buffer: up to 347 pixels per line (H40 + borders), up to 294 lines (PAL + borders) - doubled in interlaced modes
	static constexpr uint32_t MaxScreenWidth = 347;
	static constexpr uint32_t MaxScreenHeight = 588;
	static constexpr uint32_t PixelCount = MdConstants::MaxScreenWidth * MdConstants::MaxScreenHeight;

	static constexpr uint32_t WorkRamSize = 0x10000;
	static constexpr uint32_t Z80RamSize = 0x2000;
	static constexpr uint32_t VideoRamSize = 0x10000;
	static constexpr uint32_t ColorRamSize = 0x80;
	static constexpr uint32_t VsRamSize = 0x50;
};

namespace MdCpuFlags
{
	enum MdCpuFlags : uint16_t
	{
		Carry = 0x01,
		Overflow = 0x02,
		Zero = 0x04,
		Negative = 0x08,
		Extend = 0x10,

		IrqMask = 0x0700,
		Supervisor = 0x2000,
		Trace = 0x8000,
	};
}

struct MdCpuState : public BaseState
{
	uint64_t CycleCount;

	uint32_t D[8];
	uint32_t A[8]; //A[7] is the active stack pointer (USP or SSP, based on SR's S flag)
	uint32_t PC; //Address of the next opcode to execute (prefetch queue holds the words at PC and PC+2)
	uint32_t USP; //Inactive stack pointer storage (only valid while it is not the active stack pointer)
	uint32_t SSP;
	uint16_t SR;

	//Prefetch queue
	uint16_t IR;
	uint16_t IRC;
	uint16_t IRD;

	uint8_t ActiveIrqs; //Bitmask of active interrupt sources (bit N = level N)
	uint8_t IrqLevel; //Current state of the IPL pins (0-7)
	uint8_t PendingIrqLevel; //IPL value sampled at the last sampling point
	bool NmiEdge; //Level 7 is edge-triggered
	bool Stopped; //STOP instruction
	bool Halted; //Double bus/address fault
};

struct MdZ80State : public SmsCpuState
{
	//Aligned to prevent GCC/Clang from placing the fields in SmsCpuState's tail padding (keeps the layout identical to MSVC's for the UI)
	alignas(8) bool BusRequested; //68000 has requested the Z80 bus via $A11100
	bool Reset; //Reset line held via $A11200 (active when false is written)
	uint16_t BankRegister; //9-bit bank register for the $8000-$FFFF window (68000 address bits 15-23)
	uint8_t BankShiftCount;
	uint64_t MasterClock; //Master clock value the Z80 has been executed up to
};

enum class MdVdpDmaMode : uint8_t
{
	MemoryToVram = 0,
	VramFill = 2,
	VramCopy = 3
};

enum class MdVdpCode : uint8_t
{
	VramRead = 0x00,
	VramWrite = 0x01,
	CramWrite = 0x03,
	VsramRead = 0x04,
	VsramWrite = 0x05,
	CramRead = 0x08,
	Vram8BitRead = 0x0C, //Undocumented
};

struct MdVdpFifoEntry
{
	uint32_t Address;
	uint16_t Value;
	uint8_t Code;
	bool HalfWritten; //For byte-wide VRAM writes, each FIFO entry needs two access slots
};

struct MdVdpState : public BaseState
{
	uint32_t FrameCount;
	uint16_t HClock; //Current position within the scanline, in MCLK (0-3419)
	uint16_t Scanline; //Scanline number, relative to the start of the active display (0 = first active line)
	uint16_t HCounter; //Value returned by the H counter (internal 9-bit value)
	uint16_t VCounter; //Value returned by the V counter (internal 9-bit value)

	uint8_t Regs[0x20];

	//Command/address state
	uint32_t AddressReg; //17 bits
	uint8_t CodeReg; //6 bits (CD0-CD5)
	bool ControlPending; //First half of a 2-word command has been written

	uint16_t ReadBuffer;
	bool ReadBufferValid;
	bool ReadPending;

	MdVdpFifoEntry Fifo[4];
	uint8_t FifoCount;
	uint8_t FifoReadPos;
	uint16_t LastFifoValue; //Used for unused bits when reading CRAM/VSRAM

	//Status register
	bool VerticalIrqPending;
	bool HorizontalIrqPending;
	bool SpriteOverflow;
	bool SpriteCollision;
	bool OddFrame;
	bool InVerticalBlank;
	bool InHorizontalBlank;

	uint8_t HIntCounter;

	uint16_t HvLatch;
	bool HvLatched;

	//DMA
	bool DmaActive;
	bool DmaFillPending;
	uint16_t DmaFillValue;
	MdVdpDmaMode DmaMode;
	uint32_t DmaLength; //Remaining transfer count (1-0x10000)
	uint32_t DmaSource; //68000 byte address (68k->VDP) or VRAM byte address (copy)
	bool DmaCopyHasByte; //VRAM copy: byte was read, write pending
	uint8_t DmaCopyByte;

	//Decoded register values (refreshed when registers are written)
	bool DisplayEnabled;
	bool H40Mode;
	bool V30Mode;
	bool Mode5;
	bool ShadowHighlightEnabled;
	uint8_t InterlaceMode;
	bool EnableHorizontalIrq;
	bool EnableVerticalIrq;
	bool EnableDma;
	bool EnableExternalIrq;
	bool HvCounterLatchEnabled;
	bool MaskLeftColumn;
	bool LeftColumnBlank;
	uint8_t AddressIncrement;
	uint8_t BackgroundColorIndex;

	uint16_t PlaneATableAddress;
	uint16_t PlaneBTableAddress;
	uint16_t WindowTableAddress;
	uint16_t SpriteTableAddress;
	uint16_t HScrollTableAddress;

	uint8_t PlaneWidth; //In cells (32, 64 or 128)
	uint8_t PlaneHeight; //In cells (32, 64 or 128)
	uint8_t HScrollMode;
	bool VScrollColumnMode;

	uint8_t WindowHPos;
	bool WindowRight;
	uint8_t WindowVPos;
	bool WindowDown;

	bool ExtVideoEnabled; //Mode register 1, bit 0 (EXTV)
};

struct MdToneChannelState
{
	uint16_t ReloadValue;
	uint16_t Timer;
	uint8_t Output;
	uint8_t Volume;
};

struct MdNoiseChannelState
{
	uint16_t Timer;
	uint16_t Lfsr;
	uint8_t Control;
	uint8_t Output;
	uint8_t Volume;
};

struct MdPsgState
{
	MdToneChannelState Tone[3];
	MdNoiseChannelState Noise;
	uint8_t SelectedReg;
};

struct MdFmState
{
	uint8_t SelectedAddress[2];
	uint8_t Registers[2][0x100]; //Shadow copy of the register writes, for the debugger
	uint32_t BusyCounter; //In FM clocks
	uint32_t Timers[2];
};

struct MdIoPortState
{
	uint8_t Data;
	uint8_t Ctrl;
	uint8_t TxData;
	uint8_t SerialCtrl;
};

struct MdControlManagerState
{
	MdIoPortState Ports[3];
};

struct MdMemoryManagerState
{
	uint16_t OpenBus; //Last value seen on the 68000 data bus
	bool CartRamEnabled; //$A130F1 bit 0
	bool CartRamWriteProtected; //$A130F1 bit 1
	uint8_t RomBanks[8]; //$A130F3-$A130FF (Sega mapper)
	bool TmssUnlocked;
	bool BiosEnabled;
	uint32_t PendingCpuStallCycles; //68000 cycles lost to Z80 bank accesses
};

struct MdState
{
	MdCpuState Cpu;
	MdZ80State Z80;
	MdVdpState Vdp;
	MdPsgState Psg;
	MdFmState Fm;
	MdMemoryManagerState MemoryManager;
	MdControlManagerState ControlManager;
};

enum class MdIrqSource : uint8_t
{
	None = 0,
	External = 2,
	Horizontal = 4,
	Vertical = 6,
};

enum class MdZ80IrqSource
{
	None = 0,
	Vdp = 1,
};

enum class MdCartRamType : uint8_t
{
	None,
	Sram8BitOdd, //Common case: 8-bit RAM on the odd (low) byte lane
	Sram8BitEven,
	Sram16Bit,
};
