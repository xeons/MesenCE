using System;
using System.Runtime.InteropServices;

namespace Mesen.Interop;

public struct MdCpuState : BaseState
{
	public UInt64 CycleCount;

	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
	public UInt32[] D;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
	public UInt32[] A;
	public UInt32 PC;
	public UInt32 USP;
	public UInt32 SSP;
	public UInt16 SR;

	public UInt16 IR;
	public UInt16 IRC;
	public UInt16 IRD;

	public byte ActiveIrqs;
	public byte IrqLevel;
	public byte PendingIrqLevel;
	[MarshalAs(UnmanagedType.I1)] public bool NmiEdge;
	[MarshalAs(UnmanagedType.I1)] public bool Stopped;
	[MarshalAs(UnmanagedType.I1)] public bool Halted;
}

[Flags]
public enum MdCpuFlags : UInt16
{
	Carry = 0x01,
	Overflow = 0x02,
	Zero = 0x04,
	Negative = 0x08,
	Extend = 0x10,

	IrqMask = 0x0700,
	Supervisor = 0x2000,
	Trace = 0x8000,
}

//Flattened version of the C++ MdZ80State (which derives from SmsCpuState)
public struct MdZ80State : BaseState
{
	public UInt64 CycleCount;
	public UInt16 PC;
	public UInt16 SP;

	public byte A;
	public byte Flags;

	public byte B;
	public byte C;
	public byte D;
	public byte E;

	public byte H;
	public byte L;

	public byte IXL;
	public byte IXH;

	public byte IYL;
	public byte IYH;

	public byte I;
	public byte R;

	public byte AltA;
	public byte AltFlags;
	public byte AltB;
	public byte AltC;
	public byte AltD;
	public byte AltE;
	public byte AltH;
	public byte AltL;

	public byte ActiveIrqs;
	[MarshalAs(UnmanagedType.I1)] public bool NmiLevel;
	[MarshalAs(UnmanagedType.I1)] public bool NmiPending;
	[MarshalAs(UnmanagedType.I1)] public bool Halted;

	[MarshalAs(UnmanagedType.I1)] public bool IFF1;
	[MarshalAs(UnmanagedType.I1)] public bool IFF2;
	public byte IM;

	public byte FlagsChanged;
	public UInt16 WZ;

	//Tail padding of the C++ base struct (SmsCpuState is 8-byte aligned)
#pragma warning disable CS0169
	private UInt32 _basePadding;
#pragma warning restore CS0169

	[MarshalAs(UnmanagedType.I1)] public bool BusRequested;
	[MarshalAs(UnmanagedType.I1)] public bool Reset;
	public UInt16 BankRegister;
	public byte BankShiftCount;
	public UInt64 MasterClock;

	public SmsCpuState ToSmsCpuState()
	{
		return new SmsCpuState() {
			CycleCount = CycleCount, PC = PC, SP = SP, A = A, Flags = Flags,
			B = B, C = C, D = D, E = E, H = H, L = L, IXL = IXL, IXH = IXH, IYL = IYL, IYH = IYH, I = I, R = R,
			AltA = AltA, AltFlags = AltFlags, AltB = AltB, AltC = AltC, AltD = AltD, AltE = AltE, AltH = AltH, AltL = AltL,
			ActiveIrqs = ActiveIrqs, NmiLevel = NmiLevel, NmiPending = NmiPending, Halted = Halted,
			IFF1 = IFF1, IFF2 = IFF2, IM = IM, FlagsChanged = FlagsChanged, WZ = WZ
		};
	}

	public void FromSmsCpuState(SmsCpuState s)
	{
		CycleCount = s.CycleCount; PC = s.PC; SP = s.SP; A = s.A; Flags = s.Flags;
		B = s.B; C = s.C; D = s.D; E = s.E; H = s.H; L = s.L; IXL = s.IXL; IXH = s.IXH; IYL = s.IYL; IYH = s.IYH; I = s.I; R = s.R;
		AltA = s.AltA; AltFlags = s.AltFlags; AltB = s.AltB; AltC = s.AltC; AltD = s.AltD; AltE = s.AltE; AltH = s.AltH; AltL = s.AltL;
		ActiveIrqs = s.ActiveIrqs; NmiLevel = s.NmiLevel; NmiPending = s.NmiPending; Halted = s.Halted;
		IFF1 = s.IFF1; IFF2 = s.IFF2; IM = s.IM; FlagsChanged = s.FlagsChanged; WZ = s.WZ;
	}
}

public enum MdVdpDmaMode : byte
{
	MemoryToVram = 0,
	VramFill = 2,
	VramCopy = 3
}

public struct MdVdpFifoEntry
{
	public UInt32 Address;
	public UInt16 Value;
	public byte Code;
	[MarshalAs(UnmanagedType.I1)] public bool HalfWritten;
}

public struct MdVdpState : BaseState
{
	public UInt32 FrameCount;
	public UInt16 HClock;
	public UInt16 Scanline;
	public UInt16 HCounter;
	public UInt16 VCounter;

	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 0x20)]
	public byte[] Regs;

	public UInt32 AddressReg;
	public byte CodeReg;
	[MarshalAs(UnmanagedType.I1)] public bool ControlPending;

	public UInt16 ReadBuffer;
	[MarshalAs(UnmanagedType.I1)] public bool ReadBufferValid;
	[MarshalAs(UnmanagedType.I1)] public bool ReadPending;

	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
	public MdVdpFifoEntry[] Fifo;
	public byte FifoCount;
	public byte FifoReadPos;
	public UInt16 LastFifoValue;

	[MarshalAs(UnmanagedType.I1)] public bool VerticalIrqPending;
	[MarshalAs(UnmanagedType.I1)] public bool HorizontalIrqPending;
	[MarshalAs(UnmanagedType.I1)] public bool SpriteOverflow;
	[MarshalAs(UnmanagedType.I1)] public bool SpriteCollision;
	[MarshalAs(UnmanagedType.I1)] public bool OddFrame;
	[MarshalAs(UnmanagedType.I1)] public bool InVerticalBlank;
	[MarshalAs(UnmanagedType.I1)] public bool InHorizontalBlank;

	public byte HIntCounter;

	public UInt16 HvLatch;
	[MarshalAs(UnmanagedType.I1)] public bool HvLatched;

	[MarshalAs(UnmanagedType.I1)] public bool DmaActive;
	[MarshalAs(UnmanagedType.I1)] public bool DmaFillPending;
	public UInt16 DmaFillValue;
	public MdVdpDmaMode DmaMode;
	public UInt32 DmaLength;
	public UInt32 DmaSource;
	[MarshalAs(UnmanagedType.I1)] public bool DmaCopyHasByte;
	public byte DmaCopyByte;

	[MarshalAs(UnmanagedType.I1)] public bool DisplayEnabled;
	[MarshalAs(UnmanagedType.I1)] public bool H40Mode;
	[MarshalAs(UnmanagedType.I1)] public bool V30Mode;
	[MarshalAs(UnmanagedType.I1)] public bool Mode5;
	[MarshalAs(UnmanagedType.I1)] public bool ShadowHighlightEnabled;
	public byte InterlaceMode;
	[MarshalAs(UnmanagedType.I1)] public bool EnableHorizontalIrq;
	[MarshalAs(UnmanagedType.I1)] public bool EnableVerticalIrq;
	[MarshalAs(UnmanagedType.I1)] public bool EnableDma;
	[MarshalAs(UnmanagedType.I1)] public bool EnableExternalIrq;
	[MarshalAs(UnmanagedType.I1)] public bool HvCounterLatchEnabled;
	[MarshalAs(UnmanagedType.I1)] public bool MaskLeftColumn;
	[MarshalAs(UnmanagedType.I1)] public bool LeftColumnBlank;
	public byte AddressIncrement;
	public byte BackgroundColorIndex;

	public UInt16 PlaneATableAddress;
	public UInt16 PlaneBTableAddress;
	public UInt16 WindowTableAddress;
	public UInt16 SpriteTableAddress;
	public UInt16 HScrollTableAddress;

	public byte PlaneWidth;
	public byte PlaneHeight;
	public byte HScrollMode;
	[MarshalAs(UnmanagedType.I1)] public bool VScrollColumnMode;

	public byte WindowHPos;
	[MarshalAs(UnmanagedType.I1)] public bool WindowRight;
	public byte WindowVPos;
	[MarshalAs(UnmanagedType.I1)] public bool WindowDown;

	[MarshalAs(UnmanagedType.I1)] public bool ExtVideoEnabled;
}

public struct MdToneChannelState
{
	public UInt16 ReloadValue;
	public UInt16 Timer;
	public byte Output;
	public byte Volume;
}

public struct MdNoiseChannelState
{
	public UInt16 Timer;
	public UInt16 Lfsr;
	public byte Control;
	public byte Output;
	public byte Volume;
}

public struct MdPsgState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
	public MdToneChannelState[] Tone;
	public MdNoiseChannelState Noise;
	public byte SelectedReg;
}

public struct MdFmState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
	public byte[] SelectedAddress;

	//Registers[2][0x100] in C++ - index with [part * 0x100 + reg]
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 0x200)]
	public byte[] Registers;

	public UInt32 BusyCounter;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
	public UInt32[] Timers;
}

public struct MdIoPortState
{
	public byte Data;
	public byte Ctrl;
	public byte TxData;
	public byte SerialCtrl;
}

public struct MdControlManagerState
{
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 3)]
	public MdIoPortState[] Ports;
}

public struct MdMemoryManagerState
{
	public UInt16 OpenBus;
	[MarshalAs(UnmanagedType.I1)] public bool CartRamEnabled;
	[MarshalAs(UnmanagedType.I1)] public bool CartRamWriteProtected;
	[MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
	public byte[] RomBanks;
	[MarshalAs(UnmanagedType.I1)] public bool TmssUnlocked;
	[MarshalAs(UnmanagedType.I1)] public bool BiosEnabled;
	public UInt32 PendingCpuStallCycles;
}

public struct MdState : BaseState
{
	public MdCpuState Cpu;
	public MdZ80State Z80;
	public MdVdpState Vdp;
	public MdPsgState Psg;
	public MdFmState Fm;
	public MdMemoryManagerState MemoryManager;
	public MdControlManagerState ControlManager;
}
