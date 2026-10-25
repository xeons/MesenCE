#include "pch.h"
#include "MD/MdVdp.h"
#include "MD/MdConsole.h"
#include "MD/MdCpu.h"
#include "MD/MdZ80.h"
#include "MD/MdMemoryManager.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/BaseControlManager.h"
#include "Shared/RewindManager.h"
#include "Shared/EventType.h"
#include "Shared/NotificationManager.h"
#include "Shared/RenderedFrame.h"
#include "Utilities/Serializer.h"

MdVdp::LineTiming MdVdp::_timings[2] = {};
bool MdVdp::_timingsInitialized = false;

void MdVdp::InitTimings()
{
	if(_timingsInitialized) {
		return;
	}

	for(int mode = 0; mode < 2; mode++) {
		bool h40 = mode == 1;
		LineTiming& t = _timings[mode];
		t.PixelCount = h40 ? 420 : 342;
		t.SlotCount = t.PixelCount / 2;
		t.VCounterChangePixel = h40 ? 0x14A : 0x10A;
		t.ActiveStart = h40 ? 13 : 11;
		t.Width = h40 ? 320 : 256;
		t.HBlankStart = h40 ? 0x166 : 0x126;
		t.HBlankEnd = h40 ? 0x00B : 0x00A;

		uint16_t n = t.PixelCount;
		uint16_t columns = t.Width / 16;
		uint16_t fetchStart = (t.ActiveStart + n - 16) % n;

		//External access slots available during the blanking area of an active line (H40: 3, H32: 4)
		constexpr uint16_t h40BlankExt[3] = { 12, 40, 72 };
		constexpr uint16_t h32BlankExt[4] = { 12, 32, 52, 72 };

		uint32_t clock = 0;
		for(uint16_t q = 0; q < n; q++) {
			uint16_t p = (q + t.VCounterChangePixel) % n;
			if((q & 0x01) == 0) {
				uint16_t slot = q >> 1;
				t.SlotClock[slot] = (uint16_t)clock;

				SlotType type = SlotType::Other;
				uint16_t d = (p + n - fetchStart) % n;
				if(d < columns * 16) {
					//Each 2-cell column has 8 access slots: name A, ext/refresh, pattern A x2, name B, sprite, pattern B x2
					uint16_t column = d / 16;
					uint16_t sub = (d & 0x0F) >> 1;
					if(sub == 1) {
						type = (column & 0x03) == 0x03 ? SlotType::Refresh : SlotType::External;
					}
				} else {
					uint16_t e = d - columns * 16;
					if(h40) {
						for(uint16_t ext : h40BlankExt) {
							if(e == ext || e == ext + 1) {
								type = SlotType::External;
							}
						}
					} else {
						for(uint16_t ext : h32BlankExt) {
							if(e == ext || e == ext + 1) {
								type = SlotType::External;
							}
						}
					}
				}

				t.ActiveSlots[slot] = type;
				t.BlankSlots[slot] = type == SlotType::Refresh ? SlotType::Refresh : SlotType::External;
			}

			//H40 mode uses MCLK/10 for part of the HSYNC period, and MCLK/8 for the rest of the line
			uint32_t pixelClocks = (h40 && !(p >= 374 && p < 404)) ? 8 : 10;
			for(uint32_t i = 0; i < pixelClocks; i++) {
				t.PixelAtClock[clock + i] = q;
			}
			clock += pixelClocks;
		}
		t.SlotClock[t.SlotCount] = MdConstants::ClocksPerScanline;

		t.PrepareSlot = ((fetchStart + n - t.VCounterChangePixel) % n) >> 1;
		t.FinishSlot = ((((t.ActiveStart + t.Width + n - t.VCounterChangePixel) % n) + 1) >> 1) % t.SlotCount;
		t.VerticalIrqSlot = ((1 + n - t.VCounterChangePixel) % n) >> 1;
	}

	_timingsInitialized = true;
}

void MdVdp::Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80Cpu* z80, MdMemoryManager* memoryManager)
{
	_emu = emu;
	_console = console;
	_cpu = cpu;
	_z80 = z80;
	_memoryManager = memoryManager;

	InitTimings();

	_outputBuffers[0] = new uint16_t[MdConstants::PixelCount];
	_outputBuffers[1] = new uint16_t[MdConstants::PixelCount];
	_renderBuffer = new uint16_t[MdConstants::PixelCount];
	memset(_outputBuffers[0], 0, MdConstants::PixelCount * sizeof(uint16_t));
	memset(_outputBuffers[1], 0, MdConstants::PixelCount * sizeof(uint16_t));
	memset(_renderBuffer, 0, MdConstants::PixelCount * sizeof(uint16_t));
	_currentOutputBuffer = _outputBuffers[0];

	_videoRam = new uint8_t[MdConstants::VideoRamSize];
	console->InitializeRam(_videoRam, MdConstants::VideoRamSize);
	console->InitializeRam(_colorRam, MdConstants::ColorRamSize);
	console->InitializeRam(_vsRam, MdConstants::VsRamSize);
	for(uint32_t i = 0; i < MdConstants::ColorRamSize; i += 2) {
		_colorRam[i] &= 0x0E;
		_colorRam[i + 1] &= 0xEE;
	}
	for(uint32_t i = 0; i < MdConstants::VsRamSize; i += 2) {
		_vsRam[i] &= 0x07;
	}

	_emu->RegisterMemory(MemoryType::MdVideoRam, _videoRam, MdConstants::VideoRamSize);
	_emu->RegisterMemory(MemoryType::MdColorRam, _colorRam, MdConstants::ColorRamSize);
	_emu->RegisterMemory(MemoryType::MdVsRam, _vsRam, MdConstants::VsRamSize);

	_state = {};
	UpdateDecodedRegisters();
	_lineStartClock = 0;
	_slotIndex = 0;
	_lineH40 = false;
	_lineIsActive = false;

	UpdateFrameTiming();
	UpdateConfig();
}

MdVdp::~MdVdp()
{
	delete[] _videoRam;
	delete[] _outputBuffers[0];
	delete[] _outputBuffers[1];
	delete[] _renderBuffer;
}

void MdVdp::UpdateConfig()
{
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	_disableSprites = cfg.DisableSprites;
	_disablePlaneA = cfg.DisablePlaneA;
	_disablePlaneB = cfg.DisablePlaneB;
	_disableWindow = cfg.DisableWindow;
	_removeSpriteLimit = cfg.RemoveSpriteLimit;
}

void MdVdp::SetRegion(ConsoleRegion region)
{
	_region = region;
	UpdateFrameTiming();
}

void MdVdp::UpdateFrameTiming()
{
	bool pal = _region == ConsoleRegion::Pal;
	_frameV30 = _state.V30Mode;
	_frameInterlaceMode = _state.InterlaceMode;
	bool interlaced = _frameInterlaceMode == 1 || _frameInterlaceMode == 3;

	_activeLines = _frameV30 ? 240 : 224;
	_totalLines = (pal ? MdConstants::PalScanlineCount : MdConstants::NtscScanlineCount) + ((interlaced && !_state.OddFrame) ? 1 : 0);

	if(pal) {
		_vCounterJumpFrom = _frameV30 ? 0x10A : 0x102;
	} else {
		//V30 on NTSC is invalid, the V counter never jumps
		_vCounterJumpFrom = _frameV30 ? 0x1FF : 0xEA;
	}

	if(_vCounterJumpFrom >= _totalLines) {
		_vCounterJumpTo = _vCounterJumpFrom + 1;
	} else {
		_vCounterJumpTo = 0x200 - (_totalLines - _vCounterJumpFrom - 1);
	}
}

void MdVdp::Exec(uint64_t runTo)
{
	while(true) {
		const LineTiming& timing = _timings[_lineH40 ? 1 : 0];
		uint64_t lineEnd = _lineStartClock + MdConstants::ClocksPerScanline;
		uint64_t target = std::min(runTo, lineEnd);
		uint16_t offset = (uint16_t)(target - _lineStartClock);

		while(_slotIndex < timing.SlotCount && timing.SlotClock[_slotIndex] <= offset) {
			_state.HClock = timing.SlotClock[_slotIndex];
			ProcessSlot(_slotIndex);
			_slotIndex++;
		}

		if(target == lineEnd) {
			_lineStartClock = lineEnd;
			_slotIndex = 0;
			_state.HClock = 0;
			StartLine();
		} else {
			_state.HClock = offset;
		}

		if(target == runTo) {
			break;
		}
	}

	_lastMasterClock = runTo;
}

void MdVdp::ProcessSlot(uint16_t slot)
{
	const LineTiming& timing = _timings[_lineH40 ? 1 : 0];

	if(slot == timing.FinishSlot && _renderLine >= 0) {
		FinishLine();
	}

	if(slot == timing.PrepareSlot && _state.Scanline < _activeLines) {
		PrepareLine();
	}

	if(slot == timing.VerticalIrqSlot && _state.Scanline == _activeLines) {
		_state.VerticalIrqPending = true;
		UpdateIrqState();

		_z80->SetIrqSource(MdZ80IrqSource::Vdp);
		_z80IrqActive = true;
		_z80IrqEndClock = _lineStartClock + _state.HClock + 2565;

		_console->RefreshRamCheats();
	}

	if(_z80IrqActive && _lineStartClock + _state.HClock >= _z80IrqEndClock) {
		_z80->ClearIrqSource(MdZ80IrqSource::Vdp);
		_z80IrqActive = false;
	}

	SlotType type = _lineIsActive ? timing.ActiveSlots[slot] : timing.BlankSlots[slot];
	if(type == SlotType::External) {
		ProcessExternalSlot();
	}

	_emu->ProcessPpuCycle<CpuType::Md>();
}

void MdVdp::StartLine()
{
	_state.Scanline++;
	if(_state.Scanline >= _totalLines) {
		_state.Scanline = 0;
		StartFrame();
	}

	uint16_t line = _state.Scanline;
	_lineH40 = _state.H40Mode;
	_lineIsActive = _state.DisplayEnabled && (line < _activeLines || line == _totalLines - 1);

	//The H counter is decremented on lines 0 to 224 (or 240), and reloaded on all other lines
	uint16_t prevLine = line == 0 ? 0xFFFF : line - 1;
	if(prevLine <= _activeLines) {
		if(_state.HIntCounter == 0) {
			_state.HIntCounter = _state.Regs[10];
			_state.HorizontalIrqPending = true;
			UpdateIrqState();
		} else {
			_state.HIntCounter--;
		}
	} else {
		_state.HIntCounter = _state.Regs[10];
	}

	if(line == _activeLines) {
		_state.InVerticalBlank = true;
	} else if(line == _totalLines - 1) {
		_state.InVerticalBlank = false;
	}

	_state.VCounter = GetVCounter();
}

void MdVdp::StartFrame()
{
	if(_state.InterlaceMode == 1 || _state.InterlaceMode == 3) {
		_state.OddFrame = !_state.OddFrame;
	} else {
		_state.OddFrame = false;
	}

	UpdateFrameTiming();

	_currentOutputBuffer = _currentOutputBuffer == _outputBuffers[0] ? _outputBuffers[1] : _outputBuffers[0];
	_emu->ProcessEvent(EventType::StartFrame, CpuType::Md);
}

void MdVdp::EndFrame()
{
	_emu->ProcessEvent(EventType::EndFrame, CpuType::Md);
	_state.FrameCount++;

	BuildOutputFrame(_currentOutputBuffer);

	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);

	RenderedFrame frame(_currentOutputBuffer, _frameWidth, _frameHeight, 1.0, _state.FrameCount, _console->GetControlManager()->GetPortStates());
	bool rewinding = _emu->GetRewindManager()->IsRewinding();
	_emu->GetVideoDecoder()->UpdateFrame(frame, rewinding, rewinding);

	UpdateConfig();

	_emu->ProcessEndOfFrame();
	_console->ProcessEndOfFrame();
}

void MdVdp::BuildOutputFrame(uint16_t* out)
{
	_frameHeight = _activeLines * (IsInterlaceMode2() ? 2 : 1);

	uint32_t width = 256;
	for(uint32_t row = 0; row < _frameHeight; row++) {
		width = std::max<uint32_t>(width, _lineWidths[row]);
	}
	_frameWidth = width;

	for(uint32_t row = 0; row < _frameHeight; row++) {
		uint16_t* src = _renderBuffer + row * MdConstants::MaxScreenWidth;
		uint16_t* dst = out + row * width;
		uint32_t lineWidth = _lineWidths[row];
		if(lineWidth == width) {
			memcpy(dst, src, width * sizeof(uint16_t));
		} else if(lineWidth == 0) {
			memset(dst, 0, width * sizeof(uint16_t));
		} else {
			//Line was rendered in H32 mode in a frame that also contains H40 lines, stretch it
			for(uint32_t x = 0; x < width; x++) {
				dst[x] = src[x * lineWidth / width];
			}
		}
	}
}

void MdVdp::DebugSendFrame()
{
	BuildOutputFrame(_currentOutputBuffer);
	RenderedFrame frame(_currentOutputBuffer, _frameWidth, _frameHeight, 1.0, _state.FrameCount);
	_emu->GetVideoDecoder()->UpdateFrame(frame, false, false);
}

void MdVdp::ProcessExternalSlot()
{
	bool slotUsed = false;
	if(_state.FifoCount > 0) {
		slotUsed = DrainFifo();
	}

	if(_state.DmaActive) {
		if(_state.DmaMode == MdVdpDmaMode::MemoryToVram) {
			//Fetching from the 68000's bus does not use a VRAM access slot - the FIFO entries are written in later slots
			if(_state.FifoCount < 4) {
				ProcessDmaFetch();
			}
		} else if(!slotUsed && _state.FifoCount == 0) {
			ProcessDmaFillCopy();
		}
	} else if(!slotUsed && _state.ReadPending && !_state.ReadBufferValid && _state.FifoCount == 0) {
		ProcessRead();
	}
}

bool MdVdp::DrainFifo()
{
	MdVdpFifoEntry& entry = _state.Fifo[_state.FifoReadPos];
	switch(entry.Code & 0x0F) {
		case 0x01:
			//VRAM is accessed one byte at a time, a word write takes 2 slots
			if(!entry.HalfWritten) {
				WriteVramByte(entry.Address & 0xFFFF, entry.Value >> 8);
				entry.HalfWritten = true;
				return true;
			}
			WriteVramByte((entry.Address ^ 0x01) & 0xFFFF, (uint8_t)entry.Value);
			break;

		case 0x03: WriteCram(entry.Address, entry.Value); break;
		case 0x05: WriteVsram(entry.Address, entry.Value); break;
		default: break;
	}

	_state.LastFifoValue = entry.Value;
	_state.FifoReadPos = (_state.FifoReadPos + 1) & 0x03;
	_state.FifoCount--;
	return true;
}

void MdVdp::PushFifo(uint16_t value)
{
	if(_state.FifoCount >= 4) {
		//Can only happen with Z80 writes, which do not wait for the FIFO
		return;
	}

	MdVdpFifoEntry& entry = _state.Fifo[(_state.FifoReadPos + _state.FifoCount) & 0x03];
	entry.Address = _state.AddressReg;
	entry.Value = value;
	entry.Code = _state.CodeReg & 0x0F;
	entry.HalfWritten = false;
	_state.FifoCount++;

	_state.AddressReg = (_state.AddressReg + _state.AddressIncrement) & 0x1FFFF;
}

void MdVdp::ProcessDmaFetch()
{
	uint16_t value = _memoryManager->ReadDma(_state.DmaSource & 0xFFFFFF);
	PushFifo(value);

	//The source address wraps around at 128KB boundaries
	_state.DmaSource = (_state.DmaSource & 0xFE0000) | ((_state.DmaSource + 2) & 0x1FFFF);
	if(--_state.DmaLength == 0) {
		_state.DmaActive = false;
	}
	UpdateDmaRegisters();
}

void MdVdp::ProcessDmaFillCopy()
{
	if(_state.DmaMode == MdVdpDmaMode::VramFill) {
		switch(_state.CodeReg & 0x0F) {
			case 0x01: WriteVramByte(_state.AddressReg & 0xFFFF, _state.DmaFillValue >> 8); break;
			case 0x03: WriteCram(_state.AddressReg, _state.DmaFillValue); break;
			case 0x05: WriteVsram(_state.AddressReg, _state.DmaFillValue); break;
			default: break;
		}
		_state.AddressReg = (_state.AddressReg + _state.AddressIncrement) & 0x1FFFF;
		if(--_state.DmaLength == 0) {
			_state.DmaActive = false;
		}
	} else {
		//VRAM copy: read and write each take a slot
		if(!_state.DmaCopyHasByte) {
			_state.DmaCopyByte = _videoRam[_state.DmaSource & 0xFFFF];
			_state.DmaCopyHasByte = true;
			return;
		}

		WriteVramByte(_state.AddressReg & 0xFFFF, _state.DmaCopyByte);
		_state.DmaCopyHasByte = false;
		_state.DmaSource = (_state.DmaSource + 1) & 0xFFFF;
		_state.AddressReg = (_state.AddressReg + _state.AddressIncrement) & 0x1FFFF;
		if(--_state.DmaLength == 0) {
			_state.DmaActive = false;
		}
	}
	UpdateDmaRegisters();
}

void MdVdp::UpdateDmaRegisters()
{
	_state.Regs[19] = (uint8_t)_state.DmaLength;
	_state.Regs[20] = (uint8_t)(_state.DmaLength >> 8);
	if(_state.DmaMode == MdVdpDmaMode::MemoryToVram) {
		_state.Regs[21] = (uint8_t)(_state.DmaSource >> 1);
		_state.Regs[22] = (uint8_t)(_state.DmaSource >> 9);
	} else if(_state.DmaMode == MdVdpDmaMode::VramCopy) {
		_state.Regs[21] = (uint8_t)_state.DmaSource;
		_state.Regs[22] = (uint8_t)(_state.DmaSource >> 8);
	}
}

void MdVdp::ProcessRead()
{
	uint16_t addr = (uint16_t)_state.AddressReg;
	uint16_t value;
	switch(_state.CodeReg & 0x0F) {
		case 0x00: value = (_videoRam[addr & 0xFFFE] << 8) | _videoRam[(addr & 0xFFFE) + 1]; break;

		case 0x04: {
			uint8_t index = addr & 0x7E;
			if(index >= MdConstants::VsRamSize) {
				index = 0;
			}
			value = ((_vsRam[index] << 8) | _vsRam[index + 1]) | (_state.LastFifoValue & 0xF800);
			break;
		}

		case 0x08: {
			uint8_t index = addr & 0x7E;
			value = ((_colorRam[index] << 8) | _colorRam[index + 1]) | (_state.LastFifoValue & 0xF111);
			break;
		}

		case 0x0C: value = (_state.LastFifoValue & 0xFF00) | _videoRam[addr ^ 0x01]; break;

		default: value = _state.ReadBuffer; break;
	}

	_state.ReadBuffer = value;
	_state.ReadBufferValid = true;
	_state.AddressReg = (_state.AddressReg + _state.AddressIncrement) & 0x1FFFF;
}

void MdVdp::WriteVramByte(uint16_t addr, uint8_t value)
{
	_videoRam[addr] = value;

	//The VDP keeps an internal copy of the first 4 bytes of each sprite table entry (Y, size and link)
	uint16_t offset = addr - _state.SpriteTableAddress;
	if(offset < 80 * 8 && (offset & 0x04) == 0) {
		_spriteCache[(offset >> 3) * 4 + (offset & 0x03)] = value;
	}
}

void MdVdp::WriteCram(uint16_t addr, uint16_t value)
{
	uint8_t index = addr & 0x7E;
	bool drawCramDot = _renderLine >= 0 && _state.DisplayEnabled;
	uint16_t x = 0;
	if(_renderLine >= 0) {
		x = GetCurrentRenderX();
		CompositeTo(x);
	}

	_colorRam[index] = (value >> 8) & 0x0E;
	_colorRam[index + 1] = value & 0xEE;

	if(drawCramDot && x < _renderWidth && x == _compositedX) {
		//CRAM dot: the color being written is output instead of the current pixel
		_renderBuffer[_renderRow * MdConstants::MaxScreenWidth + x] = ((value >> 3) & 0x1C0) | ((value >> 2) & 0x38) | ((value >> 1) & 0x07);
		_compositedX = x + 1;
	}
}

void MdVdp::WriteVsram(uint16_t addr, uint16_t value)
{
	uint8_t index = addr & 0x7E;
	if(index < MdConstants::VsRamSize) {
		_vsRam[index] = (value >> 8) & 0x07;
		_vsRam[index + 1] = (uint8_t)value;
	}
}

uint16_t MdVdp::ReadPort(uint8_t port)
{
	switch(port & 0x1C) {
		case 0x00: {
			_state.ControlPending = false;
			uint16_t value = _state.ReadBuffer;
			if(_state.ReadPending) {
				_state.ReadBufferValid = false;
			}
			return value;
		}

		case 0x04: {
			uint16_t value = GetStatus();
			_state.ControlPending = false;
			_state.SpriteOverflow = false;
			_state.SpriteCollision = false;
			return value;
		}

		case 0x08:
		case 0x0C:
			return GetHvCounter();

		default:
			return 0;
	}
}

uint16_t MdVdp::PeekPort(uint8_t port)
{
	switch(port & 0x1C) {
		case 0x00: return _state.ReadBuffer;
		case 0x04: return GetStatus();
		case 0x08: case 0x0C: return GetHvCounter();
		default: return 0;
	}
}

void MdVdp::WritePort(uint8_t port, uint16_t value)
{
	switch(port & 0x1C) {
		case 0x00: WriteDataPort(value); break;
		case 0x04: WriteControlPort(value); break;
		default: break;
	}
}

bool MdVdp::IsCpuWriteBlocked(uint8_t port)
{
	return (port & 0x1C) == 0 && _state.FifoCount >= 4;
}

bool MdVdp::IsCpuReadBlocked(uint8_t port)
{
	return (port & 0x1C) == 0 && _state.ReadPending && !_state.ReadBufferValid;
}

bool MdVdp::IsCpuBusLocked()
{
	return _state.DmaActive && _state.DmaMode == MdVdpDmaMode::MemoryToVram;
}

void MdVdp::WriteDataPort(uint16_t value)
{
	_state.ControlPending = false;
	PushFifo(value);

	if(_state.DmaFillPending) {
		_state.DmaFillPending = false;
		_state.DmaActive = true;
		_state.DmaMode = MdVdpDmaMode::VramFill;
		_state.DmaFillValue = value;
		_state.DmaLength = (_state.Regs[19] | (_state.Regs[20] << 8));
		if(_state.DmaLength == 0) {
			_state.DmaLength = 0x10000;
		}
	}
}

void MdVdp::WriteControlPort(uint16_t value)
{
	if(_state.ControlPending) {
		_state.ControlPending = false;
		_state.AddressReg = (_state.AddressReg & 0x3FFF) | ((value & 0x07) << 14);
		_state.CodeReg = (_state.CodeReg & 0x03) | ((value >> 2) & 0x3C);

		if((_state.CodeReg & 0x20) && _state.EnableDma) {
			uint32_t length = _state.Regs[19] | (_state.Regs[20] << 8);
			if(length == 0) {
				length = 0x10000;
			}

			if((_state.Regs[23] & 0x80) == 0) {
				_state.DmaMode = MdVdpDmaMode::MemoryToVram;
				_state.DmaSource = ((_state.Regs[23] & 0x7F) << 17) | (_state.Regs[22] << 9) | (_state.Regs[21] << 1);
				_state.DmaLength = length;
				_state.DmaActive = true;
			} else if(_state.Regs[23] & 0x40) {
				_state.DmaMode = MdVdpDmaMode::VramCopy;
				_state.DmaSource = (_state.Regs[22] << 8) | _state.Regs[21];
				_state.DmaLength = length;
				_state.DmaCopyHasByte = false;
				_state.DmaActive = true;
			} else {
				//Fill starts on the next data port write
				_state.DmaFillPending = true;
			}
		}

		if((_state.CodeReg & 0x01) == 0 && !_state.DmaActive) {
			_state.ReadPending = true;
			_state.ReadBufferValid = false;
		} else {
			_state.ReadPending = false;
		}
	} else if((value & 0xC000) == 0x8000) {
		WriteRegister((value >> 8) & 0x1F, (uint8_t)value);
	} else {
		_state.ControlPending = _state.Mode5;
		_state.AddressReg = (_state.AddressReg & 0x1C000) | (value & 0x3FFF);
		_state.CodeReg = (_state.CodeReg & 0x3C) | (value >> 14);
	}
}

void MdVdp::WriteRegister(uint8_t reg, uint8_t value)
{
	if(reg >= 24 || (!_state.Mode5 && reg > 10)) {
		return;
	}

	if(_renderLine >= 0) {
		CompositeTo(GetCurrentRenderX());
	}

	bool latchEnabled = _state.HvCounterLatchEnabled;
	_state.Regs[reg] = value;
	UpdateDecodedRegisters();

	if(reg == 0) {
		if(!_state.HvCounterLatchEnabled) {
			_state.HvLatched = false;
		} else if(!latchEnabled) {
			_state.HvLatch = GetHvCounter();
			_state.HvLatched = true;
		}
	}

	if(reg == 0 || reg == 1) {
		UpdateIrqState();
	}
}

void MdVdp::UpdateDecodedRegisters()
{
	uint8_t* r = _state.Regs;

	_state.LeftColumnBlank = r[0] & 0x20;
	_state.MaskLeftColumn = r[0] & 0x20;
	_state.EnableHorizontalIrq = r[0] & 0x10;
	_state.HvCounterLatchEnabled = r[0] & 0x02;
	_state.ExtVideoEnabled = r[0] & 0x01;

	_state.DisplayEnabled = r[1] & 0x40;
	_state.EnableVerticalIrq = r[1] & 0x20;
	_state.EnableDma = r[1] & 0x10;
	_state.V30Mode = r[1] & 0x08;
	_state.Mode5 = r[1] & 0x04;

	_state.H40Mode = r[12] & 0x01;
	_state.ShadowHighlightEnabled = r[12] & 0x08;
	_state.InterlaceMode = (r[12] >> 1) & 0x03;

	_state.PlaneATableAddress = (r[2] & 0x38) << 10;
	_state.WindowTableAddress = (r[3] & (_state.H40Mode ? 0x3C : 0x3E)) << 10;
	_state.PlaneBTableAddress = (r[4] & 0x07) << 13;
	_state.SpriteTableAddress = (r[5] & (_state.H40Mode ? 0x7E : 0x7F)) << 9;
	_state.BackgroundColorIndex = r[7] & 0x3F;
	_state.HScrollTableAddress = (r[13] & 0x3F) << 10;
	_state.AddressIncrement = r[15];

	_state.EnableExternalIrq = r[11] & 0x08;
	_state.VScrollColumnMode = r[11] & 0x04;
	_state.HScrollMode = r[11] & 0x03;

	constexpr uint8_t planeSizes[4] = { 32, 64, 32, 128 };
	_state.PlaneWidth = planeSizes[r[16] & 0x03];
	_state.PlaneHeight = planeSizes[(r[16] >> 4) & 0x03];

	_state.WindowHPos = r[17] & 0x1F;
	_state.WindowRight = r[17] & 0x80;
	_state.WindowVPos = r[18] & 0x1F;
	_state.WindowDown = r[18] & 0x80;
}

void MdVdp::UpdateIrqState()
{
	if(_state.VerticalIrqPending && _state.EnableVerticalIrq) {
		_cpu->SetIrqSource(MdIrqSource::Vertical);
	} else {
		_cpu->ClearIrqSource(MdIrqSource::Vertical);
	}

	if(_state.HorizontalIrqPending && _state.EnableHorizontalIrq) {
		_cpu->SetIrqSource(MdIrqSource::Horizontal);
	} else {
		_cpu->ClearIrqSource(MdIrqSource::Horizontal);
	}
}

void MdVdp::AcknowledgeInterrupt(uint8_t level)
{
	if(level == 6) {
		_state.VerticalIrqPending = false;
	} else if(level == 4) {
		_state.HorizontalIrqPending = false;
	}
	UpdateIrqState();
}

void MdVdp::LatchHvCounter()
{
	if(_state.HvCounterLatchEnabled) {
		_state.HvLatch = GetHvCounter();
		_state.HvLatched = true;
	}
}

uint16_t MdVdp::GetStatus()
{
	uint16_t status = 0;
	if(_state.FifoCount == 0) {
		status |= 0x200;
	} else if(_state.FifoCount >= 4) {
		status |= 0x100;
	}

	if(_state.VerticalIrqPending) {
		status |= 0x80;
	}
	if(_state.SpriteOverflow) {
		status |= 0x40;
	}
	if(_state.SpriteCollision) {
		status |= 0x20;
	}
	if(_state.OddFrame) {
		status |= 0x10;
	}
	if(_state.InVerticalBlank || !_state.DisplayEnabled) {
		status |= 0x08;
	}

	const LineTiming& timing = _timings[_lineH40 ? 1 : 0];
	uint16_t hc = GetHCounter();
	_state.InHorizontalBlank = hc >= timing.HBlankStart || hc < timing.HBlankEnd;
	if(_state.InHorizontalBlank) {
		status |= 0x04;
	}

	if(_state.DmaActive) {
		status |= 0x02;
	}
	if(_region == ConsoleRegion::Pal) {
		status |= 0x01;
	}
	return status;
}

uint16_t MdVdp::GetHCounter()
{
	const LineTiming& timing = _timings[_lineH40 ? 1 : 0];
	uint16_t q = timing.PixelAtClock[std::min<uint16_t>(_state.HClock, MdConstants::ClocksPerScanline - 1)];
	uint16_t p = (q + timing.VCounterChangePixel) % timing.PixelCount;
	if(_lineH40) {
		return p <= 0x16C ? p : (p - 365 + 0x1C9);
	} else {
		return p <= 0x127 ? p : (p - 296 + 0x1D2);
	}
}

uint16_t MdVdp::GetVCounter()
{
	uint16_t line = _state.Scanline;
	if(line > _vCounterJumpFrom) {
		line = line - _vCounterJumpFrom - 1 + _vCounterJumpTo;
	}
	return line & 0x1FF;
}

uint16_t MdVdp::GetHvCounter()
{
	if(_state.HvCounterLatchEnabled && _state.HvLatched) {
		return _state.HvLatch;
	}

	uint16_t hc = GetHCounter();
	uint16_t vc = GetVCounter();
	if(_frameInterlaceMode == 3) {
		vc = (vc << 1) | ((vc >> 8) & 0x01);
	} else if(_frameInterlaceMode == 1) {
		vc = (vc & 0x1FE) | ((vc >> 8) & 0x01);
	}

	_state.HCounter = hc;
	return ((vc & 0xFF) << 8) | ((hc >> 1) & 0xFF);
}

uint16_t MdVdp::GetScanlineCount()
{
	return _totalLines;
}

uint16_t MdVdp::GetActiveDisplayStartClock()
{
	//Position (in MCLK, relative to the start of the line) of the first active display pixel
	const LineTiming& timing = _timings[_lineH40 ? 1 : 0];
	uint16_t pixel = (timing.ActiveStart + timing.PixelCount - timing.VCounterChangePixel) % timing.PixelCount;
	for(uint16_t i = 0; i < MdConstants::ClocksPerScanline; i++) {
		if(timing.PixelAtClock[i] == pixel) {
			return i;
		}
	}
	return 0;
}

uint32_t MdVdp::GetScreenWidth()
{
	return _frameWidth;
}

uint32_t MdVdp::GetScreenHeight()
{
	return _frameHeight;
}

void MdVdp::PrepareLine()
{
	if(_renderLine >= 0) {
		FinishLine();
	}

	uint16_t line = _state.Scanline;
	bool im2 = IsInterlaceMode2();
	_renderLine = line;
	_renderWidth = _lineH40 ? 320 : 256;
	_renderRow = im2 ? (line * 2 + (_state.OddFrame ? 1 : 0)) : line;
	_compositedX = 0;

	memset(_lineA, 0, sizeof(_lineA));
	memset(_lineB, 0, sizeof(_lineB));
	memset(_lineS, 0, sizeof(_lineS));

	if(!_state.Mode5 || !_state.DisplayEnabled) {
		_prevLineDotOverflow = false;
		return;
	}

	uint16_t logicalLine = im2 ? _renderRow : line;

	uint16_t hscrollAddr = _state.HScrollTableAddress;
	switch(_state.HScrollMode) {
		default:
		case 0: break;
		case 1: hscrollAddr += (line & 0x07) * 4; break;
		case 2: hscrollAddr += (line & ~0x07) * 4; break;
		case 3: hscrollAddr += line * 4; break;
	}
	uint16_t hscrollA = ((_videoRam[hscrollAddr & 0xFFFF] << 8) | _videoRam[(hscrollAddr + 1) & 0xFFFF]) & 0x3FF;
	uint16_t hscrollB = ((_videoRam[(hscrollAddr + 2) & 0xFFFF] << 8) | _videoRam[(hscrollAddr + 3) & 0xFFFF]) & 0x3FF;

	int16_t windowStart = 0;
	int16_t windowEnd = 0;
	uint16_t windowY = _state.WindowVPos * 8;
	bool windowLine = _state.WindowDown ? (line >= windowY) : (line < windowY);
	if(windowLine) {
		windowEnd = _renderWidth;
	} else {
		int16_t windowX = std::min<int16_t>(_state.WindowHPos * 16, _renderWidth);
		if(_state.WindowRight) {
			windowStart = windowX;
			windowEnd = _renderWidth;
		} else {
			windowEnd = windowX;
		}
	}

	if(!_disablePlaneB) {
		RenderPlane(_lineB, false, hscrollB, logicalLine, 0, 0);
	}
	if(!_disablePlaneA) {
		RenderPlane(_lineA, true, hscrollA, logicalLine, windowStart, windowEnd);
	}
	if(!_disableWindow && windowEnd > windowStart) {
		RenderWindow(_lineA, logicalLine, windowStart, windowEnd);
	}

	RenderSprites(logicalLine);
}

void MdVdp::RenderPlane(uint8_t* out, bool planeA, uint16_t hscroll, uint16_t logicalLine, int16_t windowStart, int16_t windowEnd)
{
	//Invalid plane size combinations behave like this on hardware (matches Genesis Plus GX's tables)
	constexpr uint8_t shiftTable[4] = { 6, 7, 0, 8 };
	constexpr uint16_t colMaskTable[4] = { 0x1F, 0x3F, 0x1F, 0x7F };
	constexpr uint16_t rowMaskTable[4] = { 0xFF, 0x1FF, 0x2FF, 0x3FF };

	bool im2 = IsInterlaceMode2();
	uint8_t sizeReg = _state.Regs[16];
	uint8_t rowShift = shiftTable[sizeReg & 0x03];
	uint16_t colMask = colMaskTable[sizeReg & 0x03];
	uint16_t rowMask = rowMaskTable[(sizeReg >> 4) & 0x03];
	uint8_t tileShift = 3;
	uint8_t tileHeight = 8;
	uint16_t vscrollMask = 0x3FF;
	if(im2) {
		rowMask = (rowMask << 1) | 0x01;
		tileShift = 4;
		tileHeight = 16;
		vscrollMask = 0x7FF;
	}

	uint16_t base = planeA ? _state.PlaneATableAddress : _state.PlaneBTableAddress;
	uint8_t planeIndex = planeA ? 0 : 1;
	uint16_t fineScroll = hscroll & 0x0F;

	auto readVsram = [this](uint8_t index) -> uint16_t {
		return (_vsRam[index * 2] << 8) | _vsRam[index * 2 + 1];
	};

	uint16_t fullVscroll = readVsram(planeIndex);
	uint16_t leftColumnVscroll = fullVscroll;
	if(_state.VScrollColumnMode) {
		//The left-most, partially shown column uses VSRAM entries 19 (ANDed) in H40 mode, and 0 in H32 mode
		leftColumnVscroll = _lineH40 ? (readVsram(38) & readVsram(39)) : 0;
	}

	//Window bug: when the window is on the left and plane A's fine scroll isn't 0, the partially shown column after the window fetches the wrong column
	bool windowBug = planeA && windowStart == 0 && windowEnd > 0 && windowEnd < _renderWidth && fineScroll != 0;

	uint32_t cachedKey = 0xFFFFFFFF;
	uint8_t tileData[4] = {};
	uint8_t attributes = 0;
	bool hflip = false;

	for(int x = 0; x < _renderWidth; x++) {
		if(x >= windowStart && x < windowEnd) {
			continue;
		}

		int fetchX = x;
		int column = ((x - (int)fineScroll + 16) >> 4) - 1;
		if(windowBug && x >= windowEnd && x < windowEnd + (int)fineScroll) {
			fetchX += 16;
			column++;
		}

		uint16_t vscroll;
		if(column < 0) {
			vscroll = leftColumnVscroll;
		} else if(_state.VScrollColumnMode) {
			vscroll = readVsram((uint8_t)(column * 2 + planeIndex));
		} else {
			vscroll = fullVscroll;
		}

		uint16_t py = (logicalLine + (vscroll & vscrollMask)) & rowMask;
		uint16_t px = (uint16_t)(fetchX - hscroll) & 0x3FF;
		uint16_t cell = (px >> 3) & colMask;
		uint16_t ntAddr = (base + ((py >> tileShift) << rowShift) + cell * 2) & 0xFFFF;

		uint32_t key = (ntAddr << 16) | (py & (tileHeight - 1));
		if(key != cachedKey) {
			cachedKey = key;
			uint16_t entry = (_videoRam[ntAddr & 0xFFFE] << 8) | _videoRam[(ntAddr & 0xFFFE) + 1];
			uint16_t row = py & (tileHeight - 1);
			if(entry & 0x1000) {
				row = tileHeight - 1 - row;
			}
			uint16_t tileIndex = entry & 0x7FF;
			uint16_t patternAddr = im2 ? (((tileIndex & 0x3FF) << 6) + row * 4) : ((tileIndex << 5) + row * 4);
			for(int i = 0; i < 4; i++) {
				tileData[i] = _videoRam[(patternAddr + i) & 0xFFFF];
			}
			hflip = entry & 0x0800;
			attributes = ((entry >> 8) & 0x80) | ((entry >> 9) & 0x30);
		}

		uint8_t col = px & 0x07;
		if(hflip) {
			col = 7 - col;
		}
		uint8_t data = tileData[col >> 1];
		uint8_t color = (col & 0x01) ? (data & 0x0F) : (data >> 4);
		out[x] = attributes | color;
	}
}

void MdVdp::RenderWindow(uint8_t* out, uint16_t logicalLine, int16_t windowStart, int16_t windowEnd)
{
	bool im2 = IsInterlaceMode2();
	uint8_t tileShift = im2 ? 4 : 3;
	uint8_t tileHeight = im2 ? 16 : 8;
	uint16_t widthCells = _lineH40 ? 64 : 32;
	uint16_t rowAddr = _state.WindowTableAddress + (logicalLine >> tileShift) * widthCells * 2;

	for(int x = windowStart; x < windowEnd; x += 8) {
		uint16_t ntAddr = (rowAddr + (x >> 3) * 2) & 0xFFFE;
		uint16_t entry = (_videoRam[ntAddr] << 8) | _videoRam[ntAddr + 1];
		uint16_t row = logicalLine & (tileHeight - 1);
		if(entry & 0x1000) {
			row = tileHeight - 1 - row;
		}
		uint16_t tileIndex = entry & 0x7FF;
		uint16_t patternAddr = im2 ? (((tileIndex & 0x3FF) << 6) + row * 4) : ((tileIndex << 5) + row * 4);
		bool hflip = entry & 0x0800;
		uint8_t attributes = ((entry >> 8) & 0x80) | ((entry >> 9) & 0x30);

		for(int i = 0; i < 8 && x + i < windowEnd; i++) {
			uint8_t col = hflip ? (7 - i) : i;
			uint8_t data = _videoRam[(patternAddr + (col >> 1)) & 0xFFFF];
			uint8_t color = (col & 0x01) ? (data & 0x0F) : (data >> 4);
			out[x + i] = attributes | color;
		}
	}
}

void MdVdp::RenderSprites(uint16_t logicalLine)
{
	bool im2 = IsInterlaceMode2();
	uint8_t maxSprites = _lineH40 ? 80 : 64;
	uint8_t maxSpritesPerLine = _lineH40 ? 20 : 16;
	uint8_t maxCellsPerLine = _lineH40 ? 40 : 32;
	uint8_t tileShift = im2 ? 4 : 3;
	uint8_t tileHeight = im2 ? 16 : 8;
	int yOffset = im2 ? 256 : 128;
	uint16_t yMask = im2 ? 0x3FF : 0x1FF;
	int line = logicalLine;

	//Phase 1: find the sprites that are on this line, using the internal sprite cache (Y/size/link)
	uint8_t found[80];
	uint8_t foundCount = 0;
	uint8_t index = 0;
	for(int visited = 0; visited < maxSprites; visited++) {
		uint8_t* cache = &_spriteCache[index * 4];
		int top = (int)(((cache[0] << 8) | cache[1]) & yMask) - yOffset;
		int height = ((cache[2] & 0x03) + 1) * tileHeight;
		if(line >= top && line < top + height) {
			if(foundCount >= maxSpritesPerLine) {
				_state.SpriteOverflow = true;
				if(!_removeSpriteLimit) {
					break;
				}
			}
			found[foundCount++] = index;
		}

		index = cache[3] & 0x7F;
		if(index == 0 || index >= maxSprites) {
			break;
		}
	}

	//Phase 2: draw the sprites, up to the per-line pixel limit
	int cellCount = 0;
	bool dotOverflow = false;
	bool masked = false;
	bool nonZeroXFound = _prevLineDotOverflow;
	for(int i = 0; i < foundCount; i++) {
		uint8_t sprIndex = found[i];
		uint8_t* cache = &_spriteCache[sprIndex * 4];
		uint16_t entryAddr = _state.SpriteTableAddress + sprIndex * 8;
		uint16_t pattern = (_videoRam[(entryAddr + 4) & 0xFFFF] << 8) | _videoRam[(entryAddr + 5) & 0xFFFF];
		uint16_t xpos = ((_videoRam[(entryAddr + 6) & 0xFFFF] << 8) | _videoRam[(entryAddr + 7) & 0xFFFF]) & 0x1FF;

		int top = (int)(((cache[0] << 8) | cache[1]) & yMask) - yOffset;
		uint8_t heightCells = (cache[2] & 0x03) + 1;
		uint8_t widthCells = ((cache[2] >> 2) & 0x03) + 1;

		//Sprite masking: a sprite at X=0 hides all following sprites, if a sprite with X!=0 was found before it on the line (or the previous line reached the pixel limit)
		if(xpos == 0) {
			if(nonZeroXFound) {
				masked = true;
			}
		} else {
			nonZeroXFound = true;
		}

		bool hflip = pattern & 0x0800;
		bool vflip = pattern & 0x1000;
		uint8_t attributes = ((pattern >> 8) & 0x80) | ((pattern >> 9) & 0x30);

		int row = line - top;
		if(vflip) {
			row = heightCells * tileHeight - 1 - row;
		}
		uint8_t cellRow = row >> tileShift;
		uint8_t pixelRow = row & (tileHeight - 1);

		for(int cell = 0; cell < widthCells; cell++) {
			if(cellCount >= maxCellsPerLine) {
				dotOverflow = true;
				if(!_removeSpriteLimit) {
					break;
				}
			}
			cellCount++;

			if(masked) {
				continue;
			}

			uint8_t cellCol = hflip ? (widthCells - 1 - cell) : cell;
			uint16_t tile = ((pattern & 0x7FF) + cellCol * heightCells + cellRow) & 0x7FF;
			uint16_t patternAddr = im2 ? (((tile & 0x3FF) << 6) + pixelRow * 4) : ((tile << 5) + pixelRow * 4);
			int startX = (int)xpos - 128 + cell * 8;
			if(startX >= _renderWidth || startX + 8 <= 0) {
				continue;
			}

			for(int px = 0; px < 8; px++) {
				int x = startX + px;
				if(x < 0 || x >= _renderWidth) {
					continue;
				}

				uint8_t col = hflip ? (7 - px) : px;
				uint8_t data = _videoRam[(patternAddr + (col >> 1)) & 0xFFFF];
				uint8_t color = (col & 0x01) ? (data & 0x0F) : (data >> 4);
				if(color == 0) {
					continue;
				}

				if(_lineS[x] & 0x0F) {
					_state.SpriteCollision = true;
				} else {
					_lineS[x] = attributes | color;
				}
			}
		}

		if(dotOverflow && !_removeSpriteLimit) {
			break;
		}
	}

	if(dotOverflow) {
		_state.SpriteOverflow = true;
	}
	_prevLineDotOverflow = dotOverflow;

	if(_disableSprites) {
		memset(_lineS, 0, sizeof(_lineS));
	}
}

uint16_t MdVdp::GetCurrentRenderX()
{
	const LineTiming& timing = _timings[_lineH40 ? 1 : 0];
	uint16_t q = timing.PixelAtClock[std::min<uint16_t>(_state.HClock, MdConstants::ClocksPerScanline - 1)];
	uint16_t p = (q + timing.VCounterChangePixel) % timing.PixelCount;
	uint16_t x = (p + timing.PixelCount - timing.ActiveStart) % timing.PixelCount;
	if(x > timing.Width + 64) {
		//Before the start of the active display
		return 0;
	}
	return std::min(x, _renderWidth);
}

void MdVdp::RefreshPalette()
{
	for(int i = 0; i < 0x40; i++) {
		uint16_t value = (_colorRam[i * 2] << 8) | _colorRam[i * 2 + 1];
		_palette[i] = ((value >> 3) & 0x1C0) | ((value >> 2) & 0x38) | ((value >> 1) & 0x07);
	}
}

void MdVdp::CompositeTo(uint16_t x)
{
	if(_renderLine < 0) {
		return;
	}

	x = std::min(x, _renderWidth);
	if(x <= _compositedX) {
		return;
	}

	RefreshPalette();
	uint16_t* out = _renderBuffer + _renderRow * MdConstants::MaxScreenWidth;
	uint8_t backdrop = _state.BackgroundColorIndex;

	if(!_state.Mode5) {
		memset(out + _compositedX, 0, (x - _compositedX) * sizeof(uint16_t));
		_compositedX = x;
		return;
	}

	if(!_state.DisplayEnabled) {
		for(uint16_t i = _compositedX; i < x; i++) {
			out[i] = _palette[backdrop];
		}
		_compositedX = x;
		return;
	}

	bool shadowHighlight = _state.ShadowHighlightEnabled;
	for(uint16_t i = _compositedX; i < x; i++) {
		if(i < 8 && _state.LeftColumnBlank) {
			out[i] = _palette[backdrop];
			continue;
		}

		uint8_t a = _lineA[i];
		uint8_t b = _lineB[i];
		uint8_t s = _lineS[i];

		uint8_t planeColor;
		bool planePriority;
		if((a & 0x80) && (a & 0x0F)) {
			planeColor = a;
			planePriority = true;
		} else if((b & 0x80) && (b & 0x0F)) {
			planeColor = b;
			planePriority = true;
		} else if(a & 0x0F) {
			planeColor = a;
			planePriority = false;
		} else if(b & 0x0F) {
			planeColor = b;
			planePriority = false;
		} else {
			planeColor = backdrop;
			planePriority = false;
		}

		bool spriteOnTop = (s & 0x0F) && ((s & 0x80) || !planePriority);

		if(!shadowHighlight) {
			out[i] = _palette[(spriteOnTop ? s : planeColor) & 0x3F];
		} else {
			//Pixels are shadowed when neither plane has its priority bit set (regardless of transparency)
			PixelMode mode = ((a & 0x80) || (b & 0x80)) ? PixelMode::Normal : PixelMode::Shadow;
			uint8_t color = planeColor & 0x3F;
			if(spriteOnTop) {
				uint8_t spriteColor = s & 0x3F;
				if(spriteColor == 0x3E) {
					//Highlight operator
					mode = mode == PixelMode::Shadow ? PixelMode::Normal : PixelMode::Highlight;
				} else if(spriteColor == 0x3F) {
					//Shadow operator
					mode = PixelMode::Shadow;
				} else {
					color = spriteColor;
					if((s & 0x80) || (spriteColor & 0x0F) == 0x0E) {
						mode = PixelMode::Normal;
					}
				}
			}
			out[i] = _palette[color] | ((uint16_t)mode << 9);
		}
	}

	_compositedX = x;
}

void MdVdp::FinishLine()
{
	CompositeTo(_renderWidth);
	_lineWidths[_renderRow] = _renderWidth;

	int16_t line = _renderLine;
	_renderLine = -1;

	if(line == _activeLines - 1) {
		EndFrame();
	}
}

void MdVdp::Serialize(Serializer& s)
{
	SV(_state.FrameCount);
	SV(_state.HClock);
	SV(_state.Scanline);
	SV(_state.HCounter);
	SV(_state.VCounter);
	SVArray(_state.Regs, 0x20);

	SV(_state.AddressReg);
	SV(_state.CodeReg);
	SV(_state.ControlPending);
	SV(_state.ReadBuffer);
	SV(_state.ReadBufferValid);
	SV(_state.ReadPending);

	for(int i = 0; i < 4; i++) {
		SVI(_state.Fifo[i].Address);
		SVI(_state.Fifo[i].Value);
		SVI(_state.Fifo[i].Code);
		SVI(_state.Fifo[i].HalfWritten);
	}
	SV(_state.FifoCount);
	SV(_state.FifoReadPos);
	SV(_state.LastFifoValue);

	SV(_state.VerticalIrqPending);
	SV(_state.HorizontalIrqPending);
	SV(_state.SpriteOverflow);
	SV(_state.SpriteCollision);
	SV(_state.OddFrame);
	SV(_state.InVerticalBlank);
	SV(_state.InHorizontalBlank);
	SV(_state.HIntCounter);
	SV(_state.HvLatch);
	SV(_state.HvLatched);

	SV(_state.DmaActive);
	SV(_state.DmaFillPending);
	SV(_state.DmaFillValue);
	SV(_state.DmaMode);
	SV(_state.DmaLength);
	SV(_state.DmaSource);
	SV(_state.DmaCopyHasByte);
	SV(_state.DmaCopyByte);

	SVArray(_videoRam, MdConstants::VideoRamSize);
	SVArray(_colorRam, MdConstants::ColorRamSize);
	SVArray(_vsRam, MdConstants::VsRamSize);
	SVArray(_spriteCache, 80 * 4);

	SV(_lastMasterClock);
	SV(_lineStartClock);
	SV(_slotIndex);
	SV(_lineH40);
	SV(_lineIsActive);
	SV(_frameV30);
	SV(_frameInterlaceMode);
	SV(_totalLines);
	SV(_activeLines);
	SV(_vCounterJumpFrom);
	SV(_vCounterJumpTo);
	SV(_z80IrqActive);
	SV(_z80IrqEndClock);

	SV(_renderLine);
	SV(_renderRow);
	SV(_renderWidth);
	SV(_compositedX);
	SV(_prevLineDotOverflow);
	SVArray(_lineA, MdConstants::MaxScreenWidth);
	SVArray(_lineB, MdConstants::MaxScreenWidth);
	SVArray(_lineS, MdConstants::MaxScreenWidth);

	if(!s.IsSaving()) {
		UpdateDecodedRegisters();
	}
}
