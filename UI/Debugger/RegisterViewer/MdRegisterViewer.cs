using Mesen.Debugger.ViewModels;
using Mesen.Interop;
using System;
using System.Collections.Generic;
using static Mesen.Debugger.ViewModels.RegEntry;

namespace Mesen.Debugger.RegisterViewer;

public class MdRegisterViewer
{
	public static List<RegisterViewerTab> GetTabs(ref MdState mdState)
	{
		List<RegisterViewerTab> tabs = new() {
			GetVdpTab(ref mdState),
			GetVdpStateTab(ref mdState),
			GetFmTab(ref mdState),
			GetPsgTab(ref mdState),
			GetMiscTab(ref mdState),
		};
		return tabs;
	}

	private static string GetCodeName(byte code)
	{
		return (code & 0x0F) switch {
			0x00 => "VRAM read",
			0x01 => "VRAM write",
			0x03 => "CRAM write",
			0x04 => "VSRAM read",
			0x05 => "VSRAM write",
			0x08 => "CRAM read",
			0x0C => "VRAM 8-bit read",
			_ => "Invalid"
		};
	}

	private static RegisterViewerTab GetVdpTab(ref MdState md)
	{
		MdVdpState vdp = md.Vdp;
		byte[] r = vdp.Regs;

		string hscrollMode = vdp.HScrollMode switch {
			0 => "Full screen",
			1 => "Invalid (first 8 lines)",
			2 => "Per cell (8 lines)",
			_ => "Per line"
		};

		string interlaceMode = vdp.InterlaceMode switch {
			0 => "Off",
			1 => "Interlace",
			2 => "Invalid",
			_ => "Interlace (double resolution)"
		};

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("$00", "Mode Set Register 1", r[0], Format.X8),
			new RegEntry("$00.0", "EXTV - External video", vdp.ExtVideoEnabled),
			new RegEntry("$00.1", "HV counter latch enabled", vdp.HvCounterLatchEnabled),
			new RegEntry("$00.4", "Horizontal IRQ enabled", vdp.EnableHorizontalIrq),
			new RegEntry("$00.5", "Blank leftmost 8 pixels", vdp.LeftColumnBlank),

			new RegEntry("$01", "Mode Set Register 2", r[1], Format.X8),
			new RegEntry("$01.2", "Mode 5 (Mega Drive mode)", vdp.Mode5),
			new RegEntry("$01.3", "V30 mode (240 lines)", vdp.V30Mode),
			new RegEntry("$01.4", "DMA enabled", vdp.EnableDma),
			new RegEntry("$01.5", "Vertical IRQ enabled", vdp.EnableVerticalIrq),
			new RegEntry("$01.6", "Display enabled", vdp.DisplayEnabled),

			new RegEntry("$02", "Plane A table address", vdp.PlaneATableAddress, Format.X16),
			new RegEntry("$03", "Window table address", vdp.WindowTableAddress, Format.X16),
			new RegEntry("$04", "Plane B table address", vdp.PlaneBTableAddress, Format.X16),
			new RegEntry("$05", "Sprite table address", vdp.SpriteTableAddress, Format.X16),
			new RegEntry("$06", "Sprite pattern generator (128KB mode)", r[6], Format.X8),
			new RegEntry("$07", "Background color", r[7], Format.X8),
			new RegEntry("$07.0-3", "Color index", vdp.BackgroundColorIndex & 0x0F),
			new RegEntry("$07.4-5", "Palette", vdp.BackgroundColorIndex >> 4),
			new RegEntry("$08", "Mode 4 horizontal scroll", r[8], Format.X8),
			new RegEntry("$09", "Mode 4 vertical scroll", r[9], Format.X8),
			new RegEntry("$0A", "Horizontal IRQ counter reload", r[10], Format.X8),

			new RegEntry("$0B", "Mode Set Register 3", r[11], Format.X8),
			new RegEntry("$0B.0-1", "Horizontal scroll mode", hscrollMode, vdp.HScrollMode),
			new RegEntry("$0B.2", "Vertical scroll mode", vdp.VScrollColumnMode ? "2-cell columns" : "Full screen", vdp.VScrollColumnMode),
			new RegEntry("$0B.3", "External IRQ enabled", vdp.EnableExternalIrq),

			new RegEntry("$0C", "Mode Set Register 4", r[12], Format.X8),
			new RegEntry("$0C.0", "H40 mode (320 pixels)", vdp.H40Mode),
			new RegEntry("$0C.1-2", "Interlace mode", interlaceMode, vdp.InterlaceMode),
			new RegEntry("$0C.3", "Shadow/highlight enabled", vdp.ShadowHighlightEnabled),

			new RegEntry("$0D", "Horizontal scroll table address", vdp.HScrollTableAddress, Format.X16),
			new RegEntry("$0E", "Plane pattern generator (128KB mode)", r[14], Format.X8),
			new RegEntry("$0F", "Auto-increment", vdp.AddressIncrement, Format.X8),

			new RegEntry("$10", "Plane size", r[16], Format.X8),
			new RegEntry("$10.0-1", "Width (cells)", vdp.PlaneWidth),
			new RegEntry("$10.4-5", "Height (cells)", vdp.PlaneHeight),

			new RegEntry("$11", "Window horizontal position", r[17], Format.X8),
			new RegEntry("$11.0-4", "Position (2-cell units)", vdp.WindowHPos),
			new RegEntry("$11.7", "Right side", vdp.WindowRight),
			new RegEntry("$12", "Window vertical position", r[18], Format.X8),
			new RegEntry("$12.0-4", "Position (cells)", vdp.WindowVPos),
			new RegEntry("$12.7", "Bottom side", vdp.WindowDown),

			new RegEntry("$13-$14", "DMA length", r[19] | (r[20] << 8), Format.X16),
			new RegEntry("$15-$17", "DMA source", r[21] | (r[22] << 8) | ((r[23] & 0x7F) << 16), Format.X24),
			new RegEntry("$17.6-7", "DMA type", (r[23] & 0x80) == 0 ? "68000 to VDP" : ((r[23] & 0x40) != 0 ? "VRAM copy" : "VRAM fill"), r[23] >> 6),
		};

		return new RegisterViewerTab("VDP Registers", entries, CpuType.Md);
	}

	private static RegisterViewerTab GetVdpStateTab(ref MdState md)
	{
		MdVdpState vdp = md.Vdp;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "State"),
			new RegEntry("", "H Clock (MCLK)", vdp.HClock),
			new RegEntry("", "Scanline", vdp.Scanline),
			new RegEntry("", "Frame Number", vdp.FrameCount),
			new RegEntry("$C00008", "V counter", vdp.VCounter, Format.X16),
			new RegEntry("$C00009", "H counter", vdp.HCounter, Format.X16),
			new RegEntry("", "HV counter latched", vdp.HvLatched),
			new RegEntry("", "HV counter latch", vdp.HvLatch, Format.X16),
			new RegEntry("", "Horizontal IRQ counter", vdp.HIntCounter),

			new RegEntry("", "Status"),
			new RegEntry("$C00004.1", "DMA busy", vdp.DmaActive),
			new RegEntry("$C00004.2", "Horizontal blank", vdp.InHorizontalBlank),
			new RegEntry("$C00004.3", "Vertical blank", vdp.InVerticalBlank),
			new RegEntry("$C00004.4", "Odd frame", vdp.OddFrame),
			new RegEntry("$C00004.5", "Sprite collision", vdp.SpriteCollision),
			new RegEntry("$C00004.6", "Sprite overflow", vdp.SpriteOverflow),
			new RegEntry("$C00004.7", "Vertical IRQ pending", vdp.VerticalIrqPending),
			new RegEntry("$C00004.8", "FIFO full", vdp.FifoCount >= 4),
			new RegEntry("$C00004.9", "FIFO empty", vdp.FifoCount == 0),
			new RegEntry("", "Horizontal IRQ pending", vdp.HorizontalIrqPending),

			new RegEntry("", "Control/Data Ports"),
			new RegEntry("", "Address register", vdp.AddressReg, Format.X24),
			new RegEntry("", "Code register", vdp.CodeReg, Format.X8),
			new RegEntry("", "Access type", GetCodeName(vdp.CodeReg), vdp.CodeReg & 0x0F),
			new RegEntry("", "Control write pending", vdp.ControlPending),
			new RegEntry("", "Read buffer", vdp.ReadBuffer, Format.X16),
			new RegEntry("", "Read buffer valid", vdp.ReadBufferValid),
			new RegEntry("", "Read pending", vdp.ReadPending),

			new RegEntry("", "FIFO"),
			new RegEntry("", "Entry count", vdp.FifoCount),
		};

		for(int i = 0; i < vdp.FifoCount && i < 4; i++) {
			MdVdpFifoEntry entry = vdp.Fifo[(vdp.FifoReadPos + i) & 0x03];
			entries.Add(new RegEntry("", $"Entry {i}", $"{GetCodeName(entry.Code)} ${entry.Address:X5} = ${entry.Value:X4}", entry.Value));
		}

		entries.AddRange(new List<RegEntry>() {
			new RegEntry("", "DMA"),
			new RegEntry("", "Active", vdp.DmaActive),
			new RegEntry("", "Fill pending", vdp.DmaFillPending),
			new RegEntry("", "Mode", vdp.DmaMode),
			new RegEntry("", "Source", vdp.DmaSource, Format.X24),
			new RegEntry("", "Remaining length", vdp.DmaLength, Format.X24),
			new RegEntry("", "Fill value", vdp.DmaFillValue, Format.X16),
		});

		return new RegisterViewerTab("VDP State", entries, CpuType.Md);
	}

	private static RegisterViewerTab GetFmTab(ref MdState md)
	{
		MdFmState fm = md.Fm;
		byte[] r = fm.Registers;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Global"),
			new RegEntry("", "Selected address (part 1)", fm.SelectedAddress[0], Format.X8),
			new RegEntry("", "Selected address (part 2)", fm.SelectedAddress[1], Format.X8),
			new RegEntry("$22", "LFO", r[0x22], Format.X8),
			new RegEntry("$22.3", "LFO enabled", (r[0x22] & 0x08) != 0),
			new RegEntry("$22.0-2", "LFO frequency", r[0x22] & 0x07),
			new RegEntry("$24-$25", "Timer A", (r[0x24] << 2) | (r[0x25] & 0x03), Format.X16),
			new RegEntry("$26", "Timer B", r[0x26], Format.X8),
			new RegEntry("$27", "Timer control / Ch3 mode", r[0x27], Format.X8),
			new RegEntry("$27.6-7", "Channel 3 mode", (r[0x27] >> 6) == 0 ? "Normal" : "Special", r[0x27] >> 6),
			new RegEntry("$28", "Key on/off (last write)", r[0x28], Format.X8),
			new RegEntry("$2A", "DAC data", r[0x2A], Format.X8),
			new RegEntry("$2B.7", "DAC enabled", (r[0x2B] & 0x80) != 0),
			new RegEntry("", "Timer A counter", fm.Timers[0]),
			new RegEntry("", "Timer B counter", fm.Timers[1]),
		};

		int[] opSlots = new int[] { 0, 8, 4, 12 };
		for(int ch = 0; ch < 6; ch++) {
			int part = ch / 3;
			int c = ch % 3;
			int b = part * 0x100;
			string prefix = part == 1 ? "B:" : "";

			int freq = r[b + 0xA0 + c] | ((r[b + 0xA4 + c] & 0x07) << 8);
			int block = (r[b + 0xA4 + c] >> 3) & 0x07;
			byte fbAlg = r[b + 0xB0 + c];
			byte panning = r[b + 0xB4 + c];

			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "Channel " + (ch + 1)),
				new RegEntry($"{prefix}${0xA4 + c:X2}.0-2/${0xA0 + c:X2}", "Frequency", freq, Format.X16),
				new RegEntry($"{prefix}${0xA4 + c:X2}.3-5", "Block", block),
				new RegEntry($"{prefix}${0xB0 + c:X2}.0-2", "Algorithm", fbAlg & 0x07),
				new RegEntry($"{prefix}${0xB0 + c:X2}.3-5", "Feedback", (fbAlg >> 3) & 0x07),
				new RegEntry($"{prefix}${0xB4 + c:X2}.7", "Left output", (panning & 0x80) != 0),
				new RegEntry($"{prefix}${0xB4 + c:X2}.6", "Right output", (panning & 0x40) != 0),
				new RegEntry($"{prefix}${0xB4 + c:X2}.4-5", "AMS", (panning >> 4) & 0x03),
				new RegEntry($"{prefix}${0xB4 + c:X2}.0-2", "FMS", panning & 0x07),
			});

			for(int op = 0; op < 4; op++) {
				int o = b + c + opSlots[op];
				entries.Add(new RegEntry(
					"",
					$"Op {op + 1}",
					$"MUL:{r[o + 0x30] & 0x0F} DT:{(r[o + 0x30] >> 4) & 0x07} TL:{r[o + 0x40] & 0x7F} AR:{r[o + 0x50] & 0x1F} RS:{r[o + 0x50] >> 6} DR:{r[o + 0x60] & 0x1F} AM:{r[o + 0x60] >> 7} SR:{r[o + 0x70] & 0x1F} SL:{r[o + 0x80] >> 4} RR:{r[o + 0x80] & 0x0F} SSG:{r[o + 0x90] & 0x0F:X}",
					r[o + 0x40]
				));
			}
		}

		return new RegisterViewerTab("FM", entries, CpuType.Md);
	}

	private static RegisterViewerTab GetPsgTab(ref MdState md)
	{
		MdPsgState psg = md.Psg;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Latched register", psg.SelectedReg),
		};

		for(int i = 0; i < 3; i++) {
			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "Tone " + (i + 1)),
				new RegEntry("", "Volume", psg.Tone[i].Volume, Format.X8),
				new RegEntry("", "Reload value", psg.Tone[i].ReloadValue, Format.X16),
				new RegEntry("", "Timer", psg.Tone[i].Timer, Format.X16),
				new RegEntry("", "Output", psg.Tone[i].Output, Format.X8),
			});
		}

		entries.AddRange(new List<RegEntry>() {
			new RegEntry("", "Noise"),
			new RegEntry("", "Volume", psg.Noise.Volume, Format.X8),
			new RegEntry("", "Control", psg.Noise.Control, Format.X8),
			new RegEntry("", "Timer", psg.Noise.Timer, Format.X16),
			new RegEntry("", "LFSR", psg.Noise.Lfsr, Format.X16),
			new RegEntry("", "Output", psg.Noise.Output, Format.X8),
		});

		return new RegisterViewerTab("PSG", entries, CpuType.Md);
	}

	private static RegisterViewerTab GetMiscTab(ref MdState md)
	{
		MdZ80State z80 = md.Z80;
		MdMemoryManagerState mm = md.MemoryManager;

		List<RegEntry> entries = new List<RegEntry>() {
			new RegEntry("", "Z80"),
			new RegEntry("$A11100", "Bus requested", z80.BusRequested),
			new RegEntry("$A11200", "Reset held", z80.Reset),
			new RegEntry("", "Bus granted to 68000", z80.BusRequested && !z80.Reset),
			new RegEntry("$6000", "Bank register", z80.BankRegister, Format.X16),
			new RegEntry("", "Bank address (68000)", (UInt32)z80.BankRegister << 15, Format.X24),

			new RegEntry("", "Cartridge"),
			new RegEntry("$A130F1.0", "Save RAM enabled", mm.CartRamEnabled),
			new RegEntry("$A130F1.1", "Save RAM write protected", mm.CartRamWriteProtected),
		};

		for(int i = 1; i < 8; i++) {
			entries.Add(new RegEntry($"$A130{0xF1 + i * 2:X2}", $"ROM bank {i} (${i * 0x80000:X6})", mm.RomBanks[i], Format.X8));
		}

		entries.AddRange(new List<RegEntry>() {
			new RegEntry("", "Bus"),
			new RegEntry("", "Open bus value", mm.OpenBus, Format.X16),
			new RegEntry("", "Pending 68000 stall cycles", mm.PendingCpuStallCycles),
		});

		string[] portNames = new string[] { "Port 1", "Port 2", "Expansion port" };
		for(int i = 0; i < 3; i++) {
			MdIoPortState port = md.ControlManager.Ports[i];
			entries.AddRange(new List<RegEntry>() {
				new RegEntry("", "I/O - " + portNames[i]),
				new RegEntry($"$A1000{3 + i * 2:X1}", "Data", port.Data, Format.X8),
				new RegEntry($"$A1000{9 + i * 2:X1}", "Control (direction)", port.Ctrl, Format.X8),
				new RegEntry($"$A1000{9 + i * 2:X1}.7", "TH interrupt enabled", (port.Ctrl & 0x80) != 0),
				new RegEntry($"$A100{0x0F + i * 6:X2}", "Serial TX data", port.TxData, Format.X8),
				new RegEntry($"$A100{0x13 + i * 6:X2}", "Serial control", port.SerialCtrl, Format.X8),
			});
		}

		return new RegisterViewerTab("Z80/IO/Cart", entries, CpuType.Md);
	}
}
