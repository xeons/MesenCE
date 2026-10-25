using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using Mesen.Utilities;
using System;
using System.Text;

namespace Mesen.Debugger.StatusViews;

public partial class MdStatusViewModel : BaseConsoleStatusViewModel
{
	[ObservableProperty] public partial UInt32 RegD0 { get; set; }
	[ObservableProperty] public partial UInt32 RegD1 { get; set; }
	[ObservableProperty] public partial UInt32 RegD2 { get; set; }
	[ObservableProperty] public partial UInt32 RegD3 { get; set; }
	[ObservableProperty] public partial UInt32 RegD4 { get; set; }
	[ObservableProperty] public partial UInt32 RegD5 { get; set; }
	[ObservableProperty] public partial UInt32 RegD6 { get; set; }
	[ObservableProperty] public partial UInt32 RegD7 { get; set; }

	[ObservableProperty] public partial UInt32 RegA0 { get; set; }
	[ObservableProperty] public partial UInt32 RegA1 { get; set; }
	[ObservableProperty] public partial UInt32 RegA2 { get; set; }
	[ObservableProperty] public partial UInt32 RegA3 { get; set; }
	[ObservableProperty] public partial UInt32 RegA4 { get; set; }
	[ObservableProperty] public partial UInt32 RegA5 { get; set; }
	[ObservableProperty] public partial UInt32 RegA6 { get; set; }
	[ObservableProperty] public partial UInt32 RegA7 { get; set; }

	[ObservableProperty] public partial UInt32 RegUSP { get; set; }
	[ObservableProperty] public partial UInt32 RegSSP { get; set; }
	[ObservableProperty] public partial UInt32 RegPC { get; set; }
	[ObservableProperty] public partial UInt16 RegSR { get; set; }

	[ObservableProperty] public partial bool FlagCarry { get; set; }
	[ObservableProperty] public partial bool FlagOverflow { get; set; }
	[ObservableProperty] public partial bool FlagZero { get; set; }
	[ObservableProperty] public partial bool FlagNegative { get; set; }
	[ObservableProperty] public partial bool FlagExtend { get; set; }
	[ObservableProperty] public partial bool FlagSupervisor { get; set; }
	[ObservableProperty] public partial bool FlagTrace { get; set; }
	[ObservableProperty] public partial int IrqMask { get; set; }

	[ObservableProperty] public partial bool FlagStopped { get; set; }
	[ObservableProperty] public partial bool FlagHalted { get; set; }
	[ObservableProperty] public partial byte IrqLevel { get; set; }

	[ObservableProperty] public partial UInt16 Scanline { get; set; }
	[ObservableProperty] public partial UInt16 HClock { get; set; }
	[ObservableProperty] public partial UInt32 FrameCount { get; set; }

	[ObservableProperty] public partial string StackPreview { get; private set; } = "";

	public MdStatusViewModel()
	{
		bool preventUpdate = false;

		this.ObserveProp([
			nameof(FlagCarry), nameof(FlagOverflow), nameof(FlagZero), nameof(FlagNegative),
			nameof(FlagExtend), nameof(FlagSupervisor), nameof(FlagTrace), nameof(IrqMask)
		], () => {
			if(!preventUpdate) {
				UpdateSrValue();
			}
		});

		this.ObserveProp(nameof(RegSR), () => {
			preventUpdate = true;
			FlagCarry = (RegSR & (UInt16)MdCpuFlags.Carry) != 0;
			FlagOverflow = (RegSR & (UInt16)MdCpuFlags.Overflow) != 0;
			FlagZero = (RegSR & (UInt16)MdCpuFlags.Zero) != 0;
			FlagNegative = (RegSR & (UInt16)MdCpuFlags.Negative) != 0;
			FlagExtend = (RegSR & (UInt16)MdCpuFlags.Extend) != 0;
			FlagSupervisor = (RegSR & (UInt16)MdCpuFlags.Supervisor) != 0;
			FlagTrace = (RegSR & (UInt16)MdCpuFlags.Trace) != 0;
			IrqMask = (RegSR >> 8) & 0x07;
			preventUpdate = false;
		});
	}

	private void UpdateSrValue()
	{
		RegSR = (UInt16)(
			(RegSR & 0x58E0) |
			(FlagCarry ? (UInt16)MdCpuFlags.Carry : 0) |
			(FlagOverflow ? (UInt16)MdCpuFlags.Overflow : 0) |
			(FlagZero ? (UInt16)MdCpuFlags.Zero : 0) |
			(FlagNegative ? (UInt16)MdCpuFlags.Negative : 0) |
			(FlagExtend ? (UInt16)MdCpuFlags.Extend : 0) |
			(FlagSupervisor ? (UInt16)MdCpuFlags.Supervisor : 0) |
			(FlagTrace ? (UInt16)MdCpuFlags.Trace : 0) |
			((Math.Clamp(IrqMask, 0, 7) & 0x07) << 8)
		);
	}

	protected override void InternalUpdateUiState()
	{
		MdCpuState cpu = DebugApi.GetCpuState<MdCpuState>(CpuType.Md);
		MdVdpState vdp = DebugApi.GetPpuState<MdVdpState>(CpuType.Md);

		UpdateCycleCount(cpu.CycleCount);

		RegD0 = cpu.D[0]; RegD1 = cpu.D[1]; RegD2 = cpu.D[2]; RegD3 = cpu.D[3];
		RegD4 = cpu.D[4]; RegD5 = cpu.D[5]; RegD6 = cpu.D[6]; RegD7 = cpu.D[7];

		RegA0 = cpu.A[0]; RegA1 = cpu.A[1]; RegA2 = cpu.A[2]; RegA3 = cpu.A[3];
		RegA4 = cpu.A[4]; RegA5 = cpu.A[5]; RegA6 = cpu.A[6]; RegA7 = cpu.A[7];

		RegUSP = cpu.USP;
		RegSSP = cpu.SSP;
		RegPC = cpu.PC;
		RegSR = cpu.SR;

		FlagStopped = cpu.Stopped;
		FlagHalted = cpu.Halted;
		IrqLevel = cpu.IrqLevel;

		Scanline = vdp.Scanline;
		HClock = vdp.HClock;
		FrameCount = vdp.FrameCount;

		StringBuilder sb = new StringBuilder();
		UInt32 sp = cpu.A[7] & 0xFFFFFE;
		byte[] stackValues = DebugApi.GetMemoryValues(MemoryType.MdMemory, sp, Math.Min(sp + 32 * 2 - 1, 0xFFFFFF));
		for(int i = 0; i + 1 < stackValues.Length; i += 2) {
			UInt16 value = (UInt16)((stackValues[i] << 8) | stackValues[i + 1]);
			sb.Append($"${value:X4} ");
		}
		StackPreview = sb.ToString();
	}

	protected override void InternalUpdateConsoleState()
	{
		MdCpuState cpu = DebugApi.GetCpuState<MdCpuState>(CpuType.Md);

		cpu.D[0] = RegD0; cpu.D[1] = RegD1; cpu.D[2] = RegD2; cpu.D[3] = RegD3;
		cpu.D[4] = RegD4; cpu.D[5] = RegD5; cpu.D[6] = RegD6; cpu.D[7] = RegD7;

		cpu.A[0] = RegA0; cpu.A[1] = RegA1; cpu.A[2] = RegA2; cpu.A[3] = RegA3;
		cpu.A[4] = RegA4; cpu.A[5] = RegA5; cpu.A[6] = RegA6; cpu.A[7] = RegA7;

		cpu.USP = RegUSP;
		cpu.SSP = RegSSP;
		cpu.PC = RegPC;
		cpu.SR = RegSR;

		cpu.Stopped = FlagStopped;
		cpu.Halted = FlagHalted;

		DebugApi.SetCpuState(cpu, CpuType.Md);
	}
}
