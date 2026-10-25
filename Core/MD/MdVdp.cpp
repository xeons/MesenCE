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

namespace
{
	struct SlotTables
	{
		bool H32Access[256] = {};
		bool H40Access[256] = {};
		bool H32BlankRefresh[256] = {};
		bool H40BlankRefresh[256] = {};

		SlotTables()
		{
			//External access slots during active display, indexed by pixel / 2 (pixel 0 = H counter 0)
			//Adapted from https://gendev.spritesmind.net/forum/viewtopic.php?t=851
			for(int slot : { 5, 13, 21, 37, 45, 53, 69, 77, 85, 101, 109, 117, 132, 133, 147, 161 }) {
				H32Access[slot] = true;
			}
			for(int slot : { 6, 14, 22, 38, 46, 54, 70, 78, 86, 102, 110, 118, 134, 142, 150, 165, 166, 190 }) {
				H40Access[slot] = true;
			}

			//Refresh slots during blanking (all other slots are available for external accesses)
			for(int slot : { 1, 33, 65, 97, 129 }) {
				H32BlankRefresh[slot] = true;
			}
			for(int slot : { 26, 58, 90, 122, 154, 204 }) {
				H40BlankRefresh[slot] = true;
			}
		}
	};

	const SlotTables _slotTables;
}

void MdVdp::Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80Cpu* z80, MdMemoryManager* memoryManager)
{
	_emu = emu;
	_console = console;
	_cpu = cpu;
	_z80 = z80;
	_memoryManager = memoryManager;

	_outputBuffers[0] = new uint16_t[MdConstants::PixelCount];
	_outputBuffers[1] = new uint16_t[MdConstants::PixelCount];
	memset(_outputBuffers[0], 0, MdConstants::PixelCount * sizeof(uint16_t));
	memset(_outputBuffers[1], 0, MdConstants::PixelCount * sizeof(uint16_t));
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
	DecodeRegs(_state.Regs, _regs);
	_latched = _regs;
	_topBorder = GetTopBorder(false);
	UpdateEventTimes();
	RefreshPalette();
	UpdateConfig();
}

MdVdp::~MdVdp()
{
	delete[] _videoRam;
	delete[] _outputBuffers[0];
	delete[] _outputBuffers[1];
}

void MdVdp::UpdateConfig()
{
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	_disableSprites = cfg.DisableSprites;
	_disablePlaneA = cfg.DisablePlaneA;
	_disablePlaneB = cfg.DisablePlaneB;
	_disableWindow = cfg.DisableWindow;
	_removeSpriteLimit = cfg.RemoveSpriteLimit;
	_showHorizontalBorder = cfg.ShowHorizontalBorder;
	_showVerticalBorder = cfg.ShowVerticalBorder;
}

void MdVdp::SetRegion(ConsoleRegion region)
{
	_isPal = region == ConsoleRegion::Pal;
	_topBorder = GetTopBorder(_regs.V30);
}

void MdVdp::DecodeRegs(uint8_t* r, Regs& regs)
{
	regs.HIntEnabled = r[0] & 0x10;
	regs.HvCounterStopped = r[0] & 0x02;
	regs.LeftColumnBlank = r[0] & 0x20;

	regs.DisplayEnabled = r[1] & 0x40;
	regs.VIntEnabled = r[1] & 0x20;
	regs.DmaEnabled = r[1] & 0x10;
	regs.V30 = r[1] & 0x08;
	regs.Mode5 = r[1] & 0x04;
	regs.Vram128k = r[1] & 0x80;

	regs.H40 = (r[12] & 0x81) != 0;
	regs.ShadowHighlight = r[12] & 0x08;
	uint8_t interlace = (r[12] >> 1) & 0x03;
	regs.InterlaceMode = interlace == 2 ? 0 : interlace;

	regs.PlaneATable = (r[2] & 0x38) << 10;
	regs.WindowTable = ((r[3] & 0x3E) << 10) & (regs.H40 ? 0xF000 : 0xF800);
	regs.PlaneBTable = (r[4] & 0x07) << 13;
	regs.SpriteTable = ((uint32_t)r[5] << 9) & (regs.H40 ? ~0x3FFu : ~0u);
	regs.BackdropColor = r[7] & 0x3F;
	regs.HIntInterval = r[10];

	regs.VScroll2Cell = r[11] & 0x04;
	regs.HScrollMode = r[11] & 0x03;

	regs.HScrollTable = (r[13] & 0x3F) << 10;
	regs.AutoIncrement = r[15];

	regs.HScrollSize = r[16] & 0x03;
	regs.VScrollSize = (r[16] >> 4) & 0x03;

	regs.WindowRight = r[17] & 0x80;
	regs.WindowX = (r[17] & 0x1F) << 1;
	regs.WindowDown = r[18] & 0x80;
	regs.WindowY = r[18] & 0x1F;
}

void MdVdp::RefreshPalette()
{
	for(int i = 0; i < 0x40; i++) {
		uint16_t value = (_colorRam[i * 2] << 8) | _colorRam[i * 2 + 1];
		_palette[i] = ((value >> 3) & 0x1C0) | ((value >> 2) & 0x38) | ((value >> 1) & 0x07);
	}
}

uint16_t MdVdp::GetTopBorder(bool v30)
{
	if(!_isPal) {
		return MdVdp::NtscTopBorder;
	}
	return v30 ? MdVdp::PalV30TopBorder : MdVdp::PalV28TopBorder;
}

uint16_t MdVdp::GetBottomBorder(bool v30)
{
	if(!_isPal) {
		return MdVdp::NtscBottomBorder;
	}
	return v30 ? MdVdp::PalV30BottomBorder : MdVdp::PalV28BottomBorder;
}

uint16_t MdVdp::GetRenderedLinesPerFrame()
{
	return _isPal ? (224 + MdVdp::PalV28TopBorder + MdVdp::PalV28BottomBorder) : (224 + MdVdp::NtscTopBorder + MdVdp::NtscBottomBorder);
}

uint16_t MdVdp::GetScanlinesInFrame()
{
	if(_isPal) {
		return MdConstants::PalScanlineCount - 1 + ((!_interlacedFrame || _interlacedOdd) ? 1 : 0);
	} else {
		return MdConstants::NtscScanlineCount + ((_interlacedFrame && _interlacedOdd) ? 1 : 0);
	}
}

uint16_t MdVdp::GetScanlineCount()
{
	return GetScanlinesInFrame();
}

uint16_t MdVdp::MclkToPixel(uint32_t mclk, bool h40)
{
	if(!h40) {
		//H32 pixel clock is always MCLK/10
		return (uint16_t)(mclk / 10);
	}

	//H40: the pixel clock is MCLK/8, except during HSYNC where it follows a pattern of
	//1 pixel at MCLK/8, 7 at MCLK/10, 2 at MCLK/9, 7 at MCLK/10 (repeated twice)
	constexpr uint32_t jumpDiff = 0x1C9 - 0x16D;
	constexpr uint32_t hsyncStart = (0x1CC - jumpDiff) * 8;
	constexpr uint32_t patternLength = 8 + 7 * 10 + 2 * 9 + 7 * 10;
	constexpr uint32_t hsyncEnd = hsyncStart + 2 * patternLength;

	if(mclk < hsyncStart) {
		return (uint16_t)(mclk / 8);
	} else if(mclk < hsyncEnd) {
		uint32_t hsyncMclk = mclk - hsyncStart;
		uint32_t patternMclk = hsyncMclk % patternLength;
		uint32_t patternPixel;
		if(patternMclk < 8) {
			patternPixel = 0;
		} else if(patternMclk < 78) {
			patternPixel = 1 + (patternMclk - 8) / 10;
		} else if(patternMclk < 96) {
			patternPixel = 8 + (patternMclk - 78) / 9;
		} else {
			patternPixel = 10 + (patternMclk - 96) / 10;
		}
		return (uint16_t)(0x1CC - jumpDiff + (hsyncMclk < patternLength ? 0 : 17) + patternPixel);
	} else {
		return (uint16_t)(0x1CC - jumpDiff + 34 + (mclk - hsyncEnd) / 8);
	}
}

uint16_t MdVdp::PixelToInternalH(uint16_t pixel, bool h40)
{
	if(h40) {
		return pixel <= 0x16C ? pixel : pixel + (0x1C9 - 0x16D);
	} else {
		return pixel <= 0x127 ? pixel : pixel + (0x1D2 - 0x128);
	}
}

uint16_t MdVdp::InternalHToPixel(uint16_t h, bool h40)
{
	if(h40) {
		return h <= 0x16C ? h : h - (0x1C9 - 0x16D);
	} else {
		return h <= 0x127 ? h : h - (0x1D2 - 0x128);
	}
}

void MdVdp::UpdateEventTimes()
{
	bool h40 = _regs.H40;
	uint16_t activeStart = h40 ? 0x01A : 0x018;
	uint16_t activeEnd = h40 ? 0x15A : 0x118;

	_events[0] = { (uint16_t)(h40 ? 0x002 : 0x001), Event::VInterrupt };
	_events[1] = { (uint16_t)(activeStart - 24), Event::ReadHScroll };
	_events[2] = { activeStart, Event::RenderLine };
	_events[3] = { (uint16_t)(activeEnd - 24), Event::FetchSpriteAttributes };
	_events[4] = { (uint16_t)(activeEnd - 16), Event::HBlankStart };
	_events[5] = { (uint16_t)(h40 ? 0x14A : 0x10A), Event::HInterrupt };
	_events[6] = { (uint16_t)(h40 ? 0x169 : 0x121), Event::LatchRegisters };
	_events[7] = { 0xFFFF, Event::None };

	std::stable_sort(std::begin(_events), std::end(_events), [](const EventTime& a, const EventTime& b) { return a.H < b.H; });
}

void MdVdp::Exec(uint64_t runTo)
{
	while(_lastMasterClock < runTo) {
		uint64_t step = std::min<uint64_t>(runTo - _lastMasterClock, MdConstants::ClocksPerScanline - _state.HClock);
		_state.HClock += (uint16_t)step;
		_lastMasterClock += step;

		if(_state.HClock >= MdConstants::ClocksPerScanline) {
			AdvanceToPixel(GetPixelsPerLine());
			_state.HClock = 0;
			AdvanceToNextLine();
		} else {
			AdvanceToPixel(MclkToPixel(_state.HClock, _regs.H40));
		}
	}
}

void MdVdp::AdvanceToPixel(uint16_t endPixel)
{
	bool h40 = _regs.H40;
	while(_pixel < endPixel) {
		if((_pixel & 0x01) == 0) {
			if(_dmaActive || _fifoCount > 0) {
				ProcessSlot(_pixel >> 1);
			}
			_emu->ProcessPpuCycle<CpuType::Md>();
		}

		uint16_t h = PixelToInternalH(_pixel, h40);
		while(h >= _events[_eventIndex].H) {
			Event evt = _events[_eventIndex].Type;
			_eventIndex++;
			ProcessEvent(evt);
		}

		_pixel++;
	}
}

void MdVdp::ProcessSlot(uint16_t slot)
{
	const bool* access = _regs.H40 ? _slotTables.H40Access : _slotTables.H32Access;
	const bool* blankRefresh = _regs.H40 ? _slotTables.H40BlankRefresh : _slotTables.H32BlankRefresh;
	bool isBlankRefresh = blankRefresh[slot];

	if(!isBlankRefresh) {
		//DMA reads must happen before the FIFO pop
		ProgressMemoryToVramDma(slot, blankRefresh);
	}

	bool blank = !_regs.DisplayEnabled || _inVblank;
	if((blank && !isBlankRefresh) || (!blank && access[slot])) {
		HandleAccessSlot();
	}

	if(!isBlankRefresh) {
		for(uint8_t i = 0; i < _fifoCount; i++) {
			FifoEntry& entry = _fifo[(_fifoPopIndex + i) & 0x03];
			if(entry.Latency > 0) {
				entry.Latency--;
			}
		}
	}
}

void MdVdp::AdvanceToNextLine()
{
	uint16_t scanlinesInFrame = GetScanlinesInFrame();

	_eventIndex = 0;
	_pixel = 0;
	_state.Scanline++;

	if(_z80IntActive) {
		//The Z80's INT line is only held during the first line of vertical blank
		_z80->ClearIrqSource(MdZ80IrqSource::Vdp);
		_z80IntActive = false;
	}

	if(_state.Scanline >= scanlinesInFrame) {
		_state.Scanline = 0;
		_vBorderForgotten = false;

		bool interlaced = _regs.InterlaceMode != 0;
		if(interlaced && !_interlacedFrame) {
			_interlacedOdd = false;
		}
		_interlacedFrame = interlaced;

		//The top border's size is latched at the start of the frame (Overdrive 2's arcade scene switches between V28 and V30 mid-frame)
		_topBorder = GetTopBorder(_regs.V30);
		_frameH40 = _regs.H40;

		_emu->ProcessEvent(EventType::StartFrame, CpuType::Md);
	} else if(_interlacedFrame) {
		uint16_t toggleLine = _regs.V30 ? 256 : 240;
		if(_state.Scanline == toggleLine) {
			_interlacedOdd = !_interlacedOdd;
		}
	}

	if(_state.Scanline == GetRenderedLinesPerFrame() - _topBorder) {
		EndFrame();
	}
}

void MdVdp::EndFrame()
{
	_emu->ProcessEvent(EventType::EndFrame, CpuType::Md);
	_state.FrameCount++;

	_frameWidth = GetCurrentScreenWidth();
	_frameHeight = GetCurrentScreenHeight();

	_emu->GetNotificationManager()->SendNotification(ConsoleNotificationType::PpuFrameDone);

	RenderedFrame frame(_currentOutputBuffer, _frameWidth, _frameHeight, 1.0, _state.FrameCount, _console->GetControlManager()->GetPortStates());
	bool rewinding = _emu->GetRewindManager()->IsRewinding();
	_emu->GetVideoDecoder()->UpdateFrame(frame, rewinding, rewinding);

	uint16_t* completedFrame = _currentOutputBuffer;
	_currentOutputBuffer = _currentOutputBuffer == _outputBuffers[0] ? _outputBuffers[1] : _outputBuffers[0];
	if(_interlacedFrame) {
		//Interlaced frames only render every other line, keep the lines from the previous field
		memcpy(_currentOutputBuffer, completedFrame, MdConstants::PixelCount * sizeof(uint16_t));
	}

	UpdateConfig();

	_emu->ProcessEndOfFrame();
	_console->ProcessEndOfFrame();
}

void MdVdp::ProcessEvent(Event evt)
{
	switch(evt) {
		case Event::VInterrupt:
			if(_state.Scanline == GetActiveScanlines(_regs)) {
				_vIntPending = true;
				UpdateIrqState();

				_z80->SetIrqSource(MdZ80IrqSource::Vdp);
				_z80IntActive = true;

				//Latch the horizontal resolution at the start of vertical blank
				_frameH40 = _regs.H40;

				_console->RefreshRamCheats();
			}
			break;

		case Event::ReadHScroll: {
			if(!_regs.DisplayEnabled || (_state.Scanline >= GetActiveScanlines(_latched) && !_vBorderForgotten)) {
				break;
			}

			//Only the lowest 8 bits of the raster line are used for H scroll lookups
			uint16_t line = GetRasterLine(_state.Scanline).Line & 0xFF;
			uint16_t addr = _regs.HScrollTable;
			switch(_regs.HScrollMode) {
				default:
				case 0: break;
				case 1: addr += 4 * (line & 0x07); break;
				case 2: addr += 32 * (line / 8); break;
				case 3: addr += 4 * line; break;
			}

			_latchedHScroll[0] = ((_videoRam[addr & 0xFFFF] << 8) | _videoRam[(addr + 1) & 0xFFFF]) & 0x3FF;
			_latchedHScroll[1] = ((_videoRam[(addr + 2) & 0xFFFF] << 8) | _videoRam[(addr + 3) & 0xFFFF]) & 0x3FF;
			break;
		}

		case Event::RenderLine: {
			if(!_spriteDisplayEnabled) {
				IncrementPixelsDisabled(_pixel);
			}

			//Sprite processing phase 1 (sprite scan for the next line)
			uint16_t nextLine = _state.Scanline == GetScanlinesInFrame() - 1 ? 0 : _state.Scanline + 1;
			ScanSprites(nextLine);

			_cramDotCount = 0;
			RenderScanline(_state.Scanline, 0);
			break;
		}

		case Event::FetchSpriteAttributes:
			//Sprite processing phase 2
			FetchSpriteAttributes();
			break;

		case Event::HBlankStart: {
			_pixelsDisabledDuringHblank = 0;
			_spriteDisplayEnabled = _regs.DisplayEnabled;
			_displayEnabledPixel = GetActiveEndH() - 16;

			uint16_t active = GetActiveScanlines(_regs);
			if(_state.Scanline == active - 1 || (_vBorderForgotten && _state.Scanline == 239)) {
				_inVblank = true;
			} else if(_state.Scanline == GetScanlinesInFrame() - 2) {
				_inVblank = false;
			}
			break;
		}

		case Event::HInterrupt: {
			memcpy(_latchedSpriteCache, _spriteCache, sizeof(_spriteCache));

			uint16_t active = GetActiveScanlines(_regs);
			if(_state.Scanline < active || _state.Scanline == GetScanlinesInFrame() - 1 || _vBorderForgotten) {
				if(_hIntCounter == 0) {
					_hIntCounter = _regs.HIntInterval;
					_hIntPending = true;
					UpdateIrqState();
				} else {
					_hIntCounter--;
				}
			} else {
				//The counter is reloaded on every line during vertical blank
				_hIntCounter = _regs.HIntInterval;
			}
			break;
		}

		case Event::LatchRegisters:
			//Most registers (and the full screen V scroll values) are latched shortly after HINT, and only take effect on the next line
			_latched = _regs;
			_latchedVScroll[0] = (_vsRam[0] << 8) | _vsRam[1];
			_latchedVScroll[1] = (_vsRam[2] << 8) | _vsRam[3];
			break;

		case Event::None:
			break;
	}
}

void MdVdp::HandleHResolutionChange()
{
	bool h40 = _regs.H40;
	_pixel = MclkToPixel(_state.HClock, h40);
	uint16_t h = PixelToInternalH(_pixel, h40);

	uint16_t effectiveLine = h >= (GetActiveEndH() - 16) ? _state.Scanline + 1 : _state.Scanline;
	uint16_t active = GetActiveScanlines(_regs);
	_inVblank = effectiveLine >= active && effectiveLine < GetScanlinesInFrame() - 1;

	UpdateEventTimes();
	_eventIndex = 0;
	while(h >= _events[_eventIndex].H) {
		_eventIndex++;
	}
}

uint16_t MdVdp::GetVCounter(uint32_t mclk, bool& vblankFlag)
{
	uint32_t divider = _regs.H40 ? 8 : 10;
	bool pastHint = mclk >= (uint32_t)GetHInterruptH() * divider;
	uint16_t scanlinesInFrame = GetScanlinesInFrame();
	uint16_t line = _state.Scanline;
	if(pastHint) {
		line = line >= scanlinesInFrame - 1 ? 0 : line + 1;
	}

	uint16_t active = _isPal ? GetActiveScanlines(_regs) : 224;
	if(!_interlacedFrame || _regs.InterlaceMode == 0) {
		uint16_t threshold = !_isPal ? 0xEA : (_regs.V30 ? 0x10A : 0x102);
		uint16_t total = _isPal ? MdConstants::PalScanlineCount : MdConstants::NtscScanlineCount;
		uint16_t counter = line <= threshold ? line : ((line - total) & 0x1FF);
		vblankFlag = counter >= active && counter != 0x1FF;
		return counter & 0xFF;
	} else {
		uint16_t threshold = !_isPal ? 0xEA : (_regs.V30 ? 0x109 : 0x101);
		uint16_t internal = line <= threshold ? line : ((line - scanlinesInFrame) & 0x1FF);
		vblankFlag = internal >= active && internal != 0x1FF;
		if(_regs.InterlaceMode == 3) {
			return ((internal << 1) & 0xFE) | ((internal >> 7) & 0x01);
		} else {
			return (internal & 0xFE) | ((internal >> 8) & 0x01);
		}
	}
}

uint16_t MdVdp::GetHvCounterAt(uint32_t mclk)
{
	if(_hvLatched) {
		return _hvLatch;
	}

	bool vblank;
	uint16_t vc = GetVCounter(mclk, vblank);
	uint16_t h = PixelToInternalH(MclkToPixel(mclk, _regs.H40), _regs.H40);
	return ((vc & 0xFF) << 8) | ((h >> 1) & 0xFF);
}

uint16_t MdVdp::GetStatus()
{
	bool vblank;
	GetVCounter(_state.HClock, vblank);
	uint16_t h = PixelToInternalH(MclkToPixel(_state.HClock, _regs.H40), _regs.H40);
	bool hblank = _regs.H40 ? !(h >= 0x00B && h < 0x166) : !(h >= 0x00A && h < 0x126);

	uint16_t status = 0;
	status |= _fifoCount == 0 ? 0x200 : 0;
	status |= _fifoCount == 4 ? 0x100 : 0;
	status |= _vIntPending ? 0x80 : 0;
	status |= _spriteOverflow ? 0x40 : 0;
	status |= _spriteCollision ? 0x20 : 0;
	status |= _interlacedOdd ? 0x10 : 0;
	status |= (vblank || !_regs.DisplayEnabled) ? 0x08 : 0;
	status |= hblank ? 0x04 : 0;
	status |= _dmaActive ? 0x02 : 0;
	status |= _isPal ? 0x01 : 0;
	return status;
}

uint16_t MdVdp::ReadPort(uint8_t port)
{
	switch(port & 0x1C) {
		case 0x00:
			return ReadDataPort();

		case 0x04: {
			uint16_t status = GetStatus();
			_spriteOverflow = false;
			_spriteCollision = false;
			_controlPending = false;
			return status;
		}

		case 0x08:
		case 0x0C:
			return GetHvCounterAt(_state.HClock);

		default:
			return 0;
	}
}

uint16_t MdVdp::PeekPort(uint8_t port)
{
	switch(port & 0x1C) {
		case 0x04: return GetStatus();
		case 0x08: case 0x0C: return GetHvCounterAt(_state.HClock);
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

void MdVdp::WriteDebugRegister(uint16_t value)
{
	_debugDisplayDisabled = (value & 0x40) != 0;
	_forcedPlane = (ForcedPlane)((value >> 7) & 0x03);
}

bool MdVdp::IsCpuWriteBlocked(uint8_t port)
{
	return (port & 0x1C) == 0 && _fifoCount >= 4;
}

bool MdVdp::IsCpuReadBlocked(uint8_t port)
{
	//Data port reads wait until all pending writes have been processed
	return (port & 0x1C) == 0 && _fifoCount > 0;
}

bool MdVdp::IsCpuBusLocked()
{
	return _dmaActive && _state.DmaMode == MdVdpDmaMode::MemoryToVram;
}

MdVdp::DataLocation MdVdp::ParseLocation(uint8_t bits, bool isWrite)
{
	switch(bits) {
		case 0x00: return DataLocation::Vram;
		case 0x02: return DataLocation::Vsram;
		case 0x01: return isWrite ? DataLocation::Cram : DataLocation::Invalid;
		case 0x04: return isWrite ? DataLocation::Invalid : DataLocation::Cram;
		case 0x06: return isWrite ? DataLocation::Invalid : DataLocation::Vram8Bit; //Undocumented 8-bit VRAM reads
		default: return DataLocation::Invalid;
	}
}

uint32_t MdVdp::Convert128kAddress(uint32_t addr)
{
	//Formula from https://plutiedev.com/mirror/kabuto-hardware-notes#128k-abuse
	return (((addr & 0x02) ^ 0x02) >> 1) | ((addr & 0x400) >> 9) | (addr & 0x3FC) | ((addr & 0x1F800) >> 1);
}

void MdVdp::WriteControlPort(uint16_t value)
{
	if(_dmaActive && _state.DmaMode == MdVdpDmaMode::MemoryToVram) {
		//The 68000 is stopped during 68000->VDP DMA, only the Z80 can get here
		return;
	}

	if(!_controlPending) {
		//The first word is always written to the address/code registers, even for register writes
		_controlAddress = (_controlAddress & ~0x3FFF) | (value & 0x3FFF);
		_dataAddress = _controlAddress;
		_isWriteMode = (value & 0x4000) != 0;
		_locationBits = (_locationBits & ~0x01) | (value >> 15);
		_location = ParseLocation(_locationBits, _isWriteMode);

		if((value & 0xC000) == 0x8000) {
			WriteRegister((value >> 8) & 0x1F, (uint8_t)value);
		} else {
			_controlPending = true;
		}
	} else {
		_controlPending = false;
		_controlAddress = (_controlAddress & 0x3FFF) | ((value & 0x07) << 14);
		_dataAddress = _controlAddress;
		_locationBits = (_locationBits & 0x01) | ((value >> 3) & 0x06);
		_location = ParseLocation(_locationBits, _isWriteMode);

		//CD5 can only be set while DMA is enabled in register #1
		if(_regs.DmaEnabled) {
			_dmaActive = (value & 0x80) != 0;
		}

		if(_dmaActive) {
			_fillDataReady = false;
			_copyOddSlot = false;
			if(_state.DmaMode == MdVdpDmaMode::MemoryToVram) {
				_dmaLatency = _location == DataLocation::Vsram ? 5 : 8;
			}
		}
	}
}

void MdVdp::WriteRegister(uint8_t reg, uint8_t value)
{
	if(!_regs.Mode5 && reg > 10) {
		//Registers above #10 can't be written in mode 4
		return;
	}
	if(reg >= 24) {
		return;
	}

	bool prevDisplayEnabled = _regs.DisplayEnabled;
	bool prevH40 = _regs.H40;
	bool prevV30 = _regs.V30;
	Regs prevRegs = _regs;

	_state.Regs[reg] = value;
	DecodeRegs(_state.Regs, _regs);
	if(_displayEnablePending) {
		_regs.DisplayEnabled = false;
	}

	switch(reg) {
		case 19: _dmaLength = (_dmaLength & 0xFF00) | value; break;
		case 20: _dmaLength = (_dmaLength & 0x00FF) | (value << 8); break;
		case 21: _dmaSource = (_dmaSource & 0xFFFFFE00) | (value << 1); break;
		case 22: _dmaSource = (_dmaSource & 0xFFFE01FF) | (value << 9); break;
		case 23:
			_dmaSource = (_dmaSource & 0x1FFFF) | ((value & 0x3F) << 17);
			switch(value & 0xC0) {
				case 0x00: _state.DmaMode = MdVdpDmaMode::MemoryToVram; break;
				case 0x40:
					//When DMD1 is 0, DMD0 is used as bit 23 of the source address
					_dmaSource |= 1 << 23;
					_state.DmaMode = MdVdpDmaMode::MemoryToVram;
					break;
				case 0x80: _state.DmaMode = MdVdpDmaMode::VramFill; break;
				case 0xC0: _state.DmaMode = MdVdpDmaMode::VramCopy; break;
			}
			break;
	}

	if(reg == 1) {
		if(!prevDisplayEnabled && _regs.DisplayEnabled && _fifoCount > 0) {
			//Display is only re-enabled once the FIFO is empty (games re-enabling the display right after a DMA depend on this)
			_regs.DisplayEnabled = false;
			_displayEnablePending = true;
		} else {
			_displayEnablePending = false;
		}
	}

	if(_regs.HvCounterStopped && !_hvLatched) {
		_hvLatch = GetHvCounterAt(_state.HClock);
		_hvLatched = true;
	} else if(!_regs.HvCounterStopped) {
		_hvLatched = false;
	}

	switch(reg) {
		case 1: RelatchRegister(reg, prevRegs.DisplayEnabled != _regs.DisplayEnabled); break;
		case 2: RelatchRegister(reg, prevRegs.PlaneATable != _regs.PlaneATable); break;
		case 3: RelatchRegister(reg, prevRegs.WindowTable != _regs.WindowTable); break;
		case 4: RelatchRegister(reg, prevRegs.PlaneBTable != _regs.PlaneBTable); break;
		case 7: RelatchRegister(reg, prevRegs.BackdropColor != _regs.BackdropColor); break;
	}

	if(reg == 1) {
		HandleDisplayEnableWrite(_regs.DisplayEnabled);

		//Switching from V30 to V28 between lines 224 and 239 makes the VDP "forget" the vertical border:
		//rendering and HINTs continue in the border area until the end of the frame
		if(prevV30 && !_regs.V30 && _state.Scanline >= 224 && _state.Scanline < 240) {
			_vBorderForgotten = true;
		}
	}

	if(prevH40 != _regs.H40) {
		HandleHResolutionChange();
	}

	if(reg == 0 || reg == 1) {
		UpdateIrqState();
	}
}

void MdVdp::RelatchRegister(uint8_t reg, bool changed)
{
	//Nametable addresses, the display enable bit and the backdrop color are not latched - they take effect immediately
	switch(reg) {
		case 1: _latched.DisplayEnabled = _regs.DisplayEnabled; break;
		case 2: _latched.PlaneATable = _regs.PlaneATable; break;
		case 3: _latched.WindowTable = _regs.WindowTable; break;
		case 4: _latched.PlaneBTable = _regs.PlaneBTable; break;
		case 7: _latched.BackdropColor = _regs.BackdropColor; break;
	}

	if(!changed) {
		return;
	}

	uint16_t renderingStart = GetActiveStartH() - 16;
	if(_pixel < renderingStart) {
		return;
	}

	uint16_t fromPixel = _pixel;
	if(reg >= 2 && reg <= 4) {
		//Nametable fetches for plane A happen at pixel 0 of each 16-pixel column, and at pixel 8 for plane B
		bool planeA = reg != 4;
		uint16_t relative = _pixel - renderingStart;
		uint16_t columnStart = relative / 16 * 16;
		uint16_t firstAffected = (planeA || (relative % 16) >= 8) ? columnStart + 16 : columnStart;
		uint16_t hscroll = _latchedHScroll[planeA ? 0 : 1];
		fromPixel = renderingStart + firstAffected + (hscroll % 16) + 16;
	}

	RenderPartialLine(fromPixel);
}

void MdVdp::RenderPartialLine(uint16_t fromPixel)
{
	if(_state.Scanline >= GetActiveScanlines(_latched)) {
		return;
	}

	uint16_t activeStart = GetActiveStartH();
	uint16_t activeEnd = GetActiveEndH();
	if(fromPixel >= activeStart && fromPixel < activeEnd) {
		RenderScanline(_state.Scanline, fromPixel - activeStart);
	}
}

void MdVdp::HandleDisplayEnableWrite(bool enabled)
{
	uint16_t renderingStart = GetActiveStartH() - 16;
	uint16_t hblankStart = GetActiveEndH() - 16;
	if(_pixel >= renderingStart && _pixel < hblankStart) {
		return;
	}

	if(!_spriteDisplayEnabled) {
		IncrementPixelsDisabled(_pixel);
	}

	_spriteDisplayEnabled = enabled;
	_displayEnabledPixel = _pixel;
}

void MdVdp::IncrementPixelsDisabled(uint16_t pixel)
{
	if(pixel >= _displayEnabledPixel) {
		_pixelsDisabledDuringHblank += pixel - _displayEnabledPixel;
	} else {
		_pixelsDisabledDuringHblank += pixel + GetPixelsPerLine() - _displayEnabledPixel;
	}
}

MdVdp::FifoEntry MdVdp::CreateFifoEntry(uint16_t value)
{
	FifoEntry entry = {};
	entry.Value = value;
	entry.Location = _location;
	entry.IsWrite = _isWriteMode;
	entry.Latency = MdVdp::InitialFifoLatency;

	if(_regs.Vram128k && (_location == DataLocation::Vram || _location == DataLocation::Invalid)) {
		//128KB mode converts the address when the entry is pushed (Overdrive 2 depends on this)
		entry.Address = Convert128kAddress(_dataAddress);
		entry.ByteWrite = true;
	} else {
		entry.Address = _dataAddress;
		entry.ByteWrite = false;
	}
	return entry;
}

void MdVdp::WriteDataPort(uint16_t value)
{
	if(_dmaActive && _state.DmaMode == MdVdpDmaMode::MemoryToVram) {
		return;
	}

	_controlPending = false;

	FifoEntry entry = CreateFifoEntry(value);
	_dataAddress = (_dataAddress + _regs.AutoIncrement) & 0x1FFFF;

	if(_fifoCount >= 4) {
		//Can only happen with Z80 writes (the 68000 waits for a free entry)
		return;
	}
	PushFifo(entry);
}

void MdVdp::PushFifo(FifoEntry entry)
{
	if(entry.IsWrite && entry.Location == DataLocation::Vram) {
		//The sprite cache is updated when entries are pushed into the FIFO
		//In 128KB mode, the LSB is written to the target address, but the MSB can still update the paired byte in the cache
		uint8_t first = entry.ByteWrite ? (uint8_t)entry.Value : (uint8_t)(entry.Value >> 8);
		uint8_t second = entry.ByteWrite ? (uint8_t)(entry.Value >> 8) : (uint8_t)entry.Value;
		UpdateSpriteCache(entry.Address, first);
		UpdateSpriteCache(entry.Address ^ 1, second);
	}

	_fifo[_fifoPushIndex] = entry;
	_fifoPushIndex = (_fifoPushIndex + 1) & 0x03;
	_fifoCount++;
}

void MdVdp::PopFifo()
{
	FifoEntry& entry = GetFifoFront();
	bool renderPartial = false;

	if(entry.IsWrite) {
		switch(entry.Location) {
			case DataLocation::Vram: {
				uint32_t addr = entry.Address & 0xFFFF;
				if(entry.ByteWrite) {
					_videoRam[addr] = (uint8_t)entry.Value;
				} else {
					_videoRam[addr] = (uint8_t)(entry.Value >> 8);
					_videoRam[addr ^ 1] = (uint8_t)entry.Value;
				}
				break;
			}

			case DataLocation::Cram:
				WriteCram((entry.Address & 0x7F) >> 1, entry.Value);
				renderPartial = true;
				break;

			case DataLocation::Vsram: {
				uint8_t index = (entry.Address & 0x7F) >> 1;
				if(index < 40) {
					WriteVsram(index, entry.Value);
					renderPartial = _latched.VScroll2Cell;
				}
				break;
			}

			default:
				break;
		}
	}

	//VRAM fill starts once an entry is popped from the FIFO
	_fillData = entry.Value;
	_fillDataReady = true;

	//VRAM writes (and writes to invalid targets) take 2 slots
	if((entry.Location == DataLocation::Vram || entry.Location == DataLocation::Invalid) && !entry.ByteWrite && !entry.SecondHalf) {
		entry.SecondHalf = true;
	} else {
		_fifoPopIndex = (_fifoPopIndex + 1) & 0x03;
		_fifoCount--;
	}

	if(_displayEnablePending && _fifoCount == 0) {
		_displayEnablePending = false;
		_regs.DisplayEnabled = true;
		_latched.DisplayEnabled = true;
		HandleDisplayEnableWrite(true);
		renderPartial = true;
	}

	if(renderPartial) {
		RenderPartialLine(_pixel);
	}
}

void MdVdp::HandleAccessSlot()
{
	if(_fifoCount > 0) {
		if(GetFifoFront().Latency == 0) {
			PopFifo();
		}
	} else if(_dmaActive) {
		switch(_state.DmaMode) {
			case MdVdpDmaMode::VramFill: ProgressVramFillDma(); break;
			case MdVdpDmaMode::VramCopy: ProgressVramCopyDma(); break;
			default: break;
		}
	}
}

void MdVdp::ProgressMemoryToVramDma(uint16_t slot, const bool* blankRefreshSlots)
{
	if(!_dmaActive || _state.DmaMode != MdVdpDmaMode::MemoryToVram) {
		return;
	}

	if(_dmaLatency > 0) {
		_dmaLatency--;
	}

	if(_fifoCount >= 4) {
		return;
	}

	//A read slot is lost after every refresh slot
	if(slot != 0 && blankRefreshSlots[slot - 1]) {
		return;
	}

	uint16_t value = _memoryManager->ReadDma(_dmaSource & 0xFFFFFF);
	IncrementDmaSource();

	FifoEntry entry = CreateFifoEntry(value);
	entry.Latency = std::max(_dmaLatency, MdVdp::InitialFifoLatency);
	PushFifo(entry);
	_dataAddress = (_dataAddress + _regs.AutoIncrement) & 0x1FFFF;

	DecrementDmaLength();
}

void MdVdp::ProgressVramFillDma()
{
	if(!_fillDataReady) {
		return;
	}

	//VRAM fill increments the source address too, even though it is not used
	IncrementDmaSource();

	switch(_location) {
		case DataLocation::Vram:
		case DataLocation::Vram8Bit: {
			uint8_t value = (uint8_t)(_fillData >> 8);
			uint32_t addr = (_dataAddress ^ 1) & 0xFFFF;
			_videoRam[addr] = value;
			UpdateSpriteCache(addr, value);
			break;
		}

		case DataLocation::Cram:
			//CRAM/VSRAM fills use the value from the next FIFO slot instead of the fill data
			WriteCram((_dataAddress & 0x7F) >> 1, GetNextSlotWord());
			break;

		case DataLocation::Vsram: {
			uint8_t index = (_dataAddress & 0x7F) >> 1;
			if(index < 40) {
				WriteVsram(index, GetNextSlotWord());
			}
			break;
		}

		default:
			break;
	}

	_dataAddress = (_dataAddress + _regs.AutoIncrement) & 0x1FFFF;
	DecrementDmaLength();
}

void MdVdp::ProgressVramCopyDma()
{
	//Each byte takes 2 slots (read + write)
	_copyOddSlot = !_copyOddSlot;
	if(_copyOddSlot) {
		return;
	}

	uint32_t src = ((_dmaSource >> 1) ^ 1) & 0xFFFF;
	IncrementDmaSource();

	uint32_t dst = (_dataAddress ^ 1) & 0xFFFF;
	_dataAddress = (_dataAddress + _regs.AutoIncrement) & 0x1FFFF;

	uint8_t value = _videoRam[src];
	_videoRam[dst] = value;
	UpdateSpriteCache(dst, value);

	DecrementDmaLength();
}

void MdVdp::IncrementDmaSource()
{
	//The DMA source address wraps within a 128KB block
	_dmaSource = (_dmaSource & ~0x1FFFF) | ((_dmaSource + 2) & 0x1FFFF);
}

void MdVdp::DecrementDmaLength()
{
	//A length of 0 behaves like 65536
	_dmaLength--;

	_state.Regs[19] = (uint8_t)_dmaLength;
	_state.Regs[20] = (uint8_t)(_dmaLength >> 8);
	_state.Regs[21] = (uint8_t)(_dmaSource >> 1);
	_state.Regs[22] = (uint8_t)(_dmaSource >> 9);
	_state.Regs[23] = (_state.Regs[23] & 0xC0) | ((_dmaSource >> 17) & 0x3F);

	if(_dmaLength == 0) {
		_dmaActive = false;
	}
}

void MdVdp::UpdateSpriteCache(uint32_t addr, uint8_t value)
{
	if(addr & 0x04) {
		//Only the first 4 bytes of each entry are cached
		return;
	}

	uint32_t tableStart = _regs.SpriteTable;
	uint32_t tableEnd = tableStart + 8 * (_regs.H40 ? 80 : 64);
	if(addr < tableStart || addr >= tableEnd) {
		return;
	}

	uint32_t index = (addr - tableStart) / 8;
	switch(addr & 0x03) {
		case 0: _spriteCache[index * 4] = value & 0x03; break;
		case 1: _spriteCache[index * 4 + 1] = value; break;
		case 2: _spriteCache[index * 4 + 2] = value; break;
		case 3: _spriteCache[index * 4 + 3] = value & 0x7F; break;
	}
}

void MdVdp::WriteCram(uint8_t index, uint16_t value)
{
	_colorRam[index * 2] = (value >> 8) & 0x0E;
	_colorRam[index * 2 + 1] = value & 0xEE;
	_palette[index] = ((value >> 3) & 0x1C0) | ((value >> 2) & 0x38) | ((value >> 1) & 0x07);

	//CRAM dots: the color being written is output at the current position while the display is active
	if(!_inVblank && _regs.DisplayEnabled && _state.Scanline < GetActiveScanlines(_latched) && _cramDotCount < 32) {
		uint16_t activeStart = GetActiveStartH();
		if(_pixel >= activeStart && _pixel < GetActiveEndH()) {
			_cramDots[_cramDotCount++] = { (uint16_t)(_pixel - activeStart), _palette[index] };
		}
	}
}

void MdVdp::WriteVsram(uint8_t index, uint16_t value)
{
	_vsRam[index * 2] = (value >> 8) & 0x07;
	_vsRam[index * 2 + 1] = (uint8_t)value;
}

uint16_t MdVdp::ReadDataPort()
{
	_controlPending = false;

	if(_isWriteMode) {
		return 0xFFFF;
	}

	uint16_t value;
	switch(_location) {
		case DataLocation::Vram:
			if(_regs.Vram128k) {
				uint8_t b = _videoRam[Convert128kAddress(_dataAddress) & 0xFFFF];
				value = (b << 8) | b;
			} else {
				uint32_t addr = _dataAddress & 0xFFFE;
				value = (_videoRam[addr] << 8) | _videoRam[addr + 1];
			}
			break;

		case DataLocation::Vram8Bit:
			value = (GetNextSlotWord() & 0xFF00) | _videoRam[(_dataAddress & 0xFFFF) ^ 1];
			break;

		case DataLocation::Cram: {
			uint8_t index = (_dataAddress & 0x7F) & ~0x01;
			value = (_colorRam[index] << 8) | _colorRam[index + 1];
			break;
		}

		case DataLocation::Vsram: {
			uint8_t index = (_dataAddress & 0x7F) & ~0x01;
			if(index >= MdConstants::VsRamSize) {
				index = 0;
			}
			value = (_vsRam[index] << 8) | _vsRam[index + 1];
			break;
		}

		default:
			value = 0xFFFF;
			break;
	}

	_dataAddress = (_dataAddress + _regs.AutoIncrement) & 0x1FFFF;

	//CRAM is 9-bit and VSRAM is 11-bit - the other bits come from the next FIFO slot
	switch(_location) {
		case DataLocation::Cram: return (value & 0x0EEE) | (GetNextSlotWord() & ~0x0EEE);
		case DataLocation::Vsram: return (value & 0x07FF) | (GetNextSlotWord() & ~0x07FF);
		default: return value;
	}
}

void MdVdp::UpdateIrqState()
{
	if(_vIntPending && _regs.VIntEnabled) {
		_cpu->SetIrqSource(MdIrqSource::Vertical);
	} else {
		_cpu->ClearIrqSource(MdIrqSource::Vertical);
	}

	if(_hIntPending && _regs.HIntEnabled) {
		_cpu->SetIrqSource(MdIrqSource::Horizontal);
	} else {
		_cpu->ClearIrqSource(MdIrqSource::Horizontal);
	}
}

void MdVdp::AcknowledgeInterrupt(uint8_t level)
{
	if(level == 6) {
		_vIntPending = false;
	} else if(level == 4) {
		_hIntPending = false;
	}
	UpdateIrqState();
}

void MdVdp::LatchHvCounter()
{
	if(_regs.HvCounterStopped) {
		_hvLatched = false;
		_hvLatch = GetHvCounterAt(_state.HClock);
		_hvLatched = true;
	}
}

uint16_t MdVdp::GetActiveDisplayStartClock()
{
	return GetActiveStartH() * (_regs.H40 ? 8 : 10);
}

uint32_t MdVdp::GetCurrentScreenWidth()
{
	uint32_t active = _frameH40 ? 320 : 256;
	if(_showHorizontalBorder) {
		return GetLeftBorder(_frameH40) + active + MdVdp::RightBorder;
	}
	return active;
}

uint32_t MdVdp::GetCurrentScreenHeight()
{
	uint32_t height = _showVerticalBorder ? GetRenderedLinesPerFrame() : GetActiveScanlines(_regs);
	return _interlacedFrame ? height * 2 : height;
}

uint32_t MdVdp::GetScreenWidth()
{
	return _frameWidth;
}

uint32_t MdVdp::GetScreenHeight()
{
	return _frameHeight;
}

MdVdp::RasterLine MdVdp::GetRasterLine(uint16_t scanline)
{
	uint16_t active = GetActiveScanlines(_latched);
	uint16_t scanlinesInFrame = GetScanlinesInFrame();
	uint16_t topBorder = GetTopBorder(_latched.V30);

	if(scanline < active) {
		return { scanline, false };
	} else if(scanline >= scanlinesInFrame - topBorder) {
		//Top border - the last line of the border is raster line 511
		return { (uint16_t)(512 - (scanlinesInFrame - scanline)), true };
	} else {
		//Bottom border and vertical blank
		return { scanline, true };
	}
}

int32_t MdVdp::GetFrameBufferRow(RasterLine line)
{
	if(_showVerticalBorder) {
		if(line.Line >= 512 - _topBorder) {
			return line.Line - (512 - _topBorder);
		} else if(line.Line < GetRenderedLinesPerFrame() - _topBorder) {
			return line.Line + _topBorder;
		}
		return -1;
	}
	return line.InVBorder ? -1 : line.Line;
}

void MdVdp::RenderScanline(uint16_t scanline, uint16_t startPixel)
{
	if(startPixel >= GetActivePixels(_latched) - 16) {
		//Ignore writes that occur very close to the end of the active display
		return;
	}

	RefreshPalette();

	RasterLine line = GetRasterLine(scanline);
	int32_t row = GetFrameBufferRow(line);
	uint8_t interlaceMode = _interlacedFrame ? _latched.InterlaceMode : 0;
	bool odd = _interlacedFrame && _interlacedOdd;

	switch(interlaceMode) {
		default:
			DoRenderScanline(scanline, line, startPixel, row, false);
			break;

		case 1:
			DoRenderScanline(scanline, line, startPixel, row >= 0 ? row * 2 + (odd ? 1 : 0) : -1, false);
			break;

		case 3:
			if(!odd) {
				DoRenderScanline(scanline, { (uint16_t)((line.Line * 2) & 0x1FF), line.InVBorder }, startPixel, row >= 0 ? row * 2 : -1, false);
			} else {
				DoRenderScanline(scanline, { (uint16_t)((line.Line * 2 + 1) & 0x1FF), line.InVBorder }, startPixel, row >= 0 ? row * 2 + 1 : -1, true);
			}
			break;
	}
}

void MdVdp::DoRenderScanline(uint16_t scanline, RasterLine line, uint16_t startPixel, int32_t row, bool oddLine)
{
	SpriteBuffers& sprites = oddLine ? _spritesOdd : _sprites;

	if(!_regs.DisplayEnabled) {
		if(row >= 0) {
			FillRow(row, startPixel, GetBackdropIndex());
		}

		//Clear the sprite line buffer in case the display gets enabled during the active display
		memset(sprites.Pixels, 0, sizeof(sprites.Pixels));
		return;
	}

	uint16_t activeLines = GetActiveScanlines(_latched);
	bool bgForced = _forcedPlane == ForcedPlane::PlaneA || _forcedPlane == ForcedPlane::PlaneB;

	if(startPixel == 0) {
		//Sprite pixels are not rendered inside the vertical border, except on the line immediately after the active display
		if(!line.InVBorder || _vBorderForgotten || line.Line == activeLines) {
			RenderSpritePixels(line, sprites);
		}

		if(_showHorizontalBorder && bgForced) {
			//The previous line's right border depends on the sprite tiles fetched for the current line
			RasterLine prevLine = line.Line == 0 ? RasterLine { 511, true } : RasterLine { (uint16_t)(line.Line - 1), (uint16_t)(line.Line - 1) >= activeLines };
			if(!prevLine.InVBorder || _vBorderForgotten || prevLine.Line == 511) {
				int32_t prevRow = GetFrameBufferRow(prevLine);
				if(prevRow >= 0) {
					RenderRightBorder(prevRow);
				}
			}
		}
	}

	if(row < 0) {
		return;
	}

	if(line.InVBorder && !_vBorderForgotten && line.Line != 511) {
		RenderVerticalBorderLine(scanline, row, startPixel);
		return;
	}

	int16_t activePixels = (int16_t)GetActivePixels(_latched);
	int16_t startCol;
	int16_t endCol;
	int16_t pixelOffset;
	if(_showHorizontalBorder) {
		int16_t leftBorder = (int16_t)GetLeftBorder(_latched.H40);
		startCol = startPixel == 0 ? 0 : (int16_t)startPixel + leftBorder;
		endCol = leftBorder + activePixels + MdVdp::RightBorder;
		pixelOffset = leftBorder;
	} else {
		startCol = (int16_t)startPixel;
		endCol = activePixels;
		pixelOffset = 0;
	}

	RenderBackgrounds(line.Line, startCol, endCol, pixelOffset);
	RenderWindow(line.Line, startCol, endCol, pixelOffset);
	MergeLayers(line, row, startCol, endCol, pixelOffset, oddLine);

	if(_showHorizontalBorder) {
		RenderLeftBorder(row);
		if(!bgForced) {
			RenderRightBorder(row);
		}
	}

	for(uint8_t i = 0; i < _cramDotCount; i++) {
		uint32_t width = GetCurrentScreenWidth();
		uint32_t col = _cramDots[i].X + pixelOffset;
		if(col < width && row * width + col < MdConstants::PixelCount) {
			_currentOutputBuffer[row * width + col] = _cramDots[i].Color;
		}
	}
}

void MdVdp::FillRow(uint32_t row, uint16_t startPixel, uint8_t colorIndex)
{
	uint32_t width = GetCurrentScreenWidth();
	uint32_t leftBorder = _showHorizontalBorder ? GetLeftBorder(_frameH40) : 0;
	uint32_t startCol = startPixel == 0 ? 0 : startPixel + leftBorder;
	for(uint32_t col = startCol; col < width; col++) {
		SetPixel(row, col, colorIndex);
	}
}

void MdVdp::SetPixel(uint32_t row, uint32_t col, uint8_t colorIndex, PixelMode mode)
{
	uint32_t width = GetCurrentScreenWidth();
	uint32_t offset = row * width + col;
	if(col < width && offset < MdConstants::PixelCount) {
		_currentOutputBuffer[offset] = _palette[colorIndex & 0x3F] | ((uint16_t)mode << 9);
	}
}

uint16_t MdVdp::ReadVScroll(uint8_t plane, int16_t column)
{
	uint16_t vscroll;
	if(!_latched.VScroll2Cell) {
		vscroll = _latchedVScroll[plane];
	} else {
		auto readVsram = [this](uint8_t index) -> uint16_t { return (_vsRam[index * 2] << 8) | _vsRam[index * 2 + 1]; };
		int16_t columns = (int16_t)(GetActivePixels(_latched) / 16);
		if(column < 0) {
			//Column -1 uses VSRAM[$4C] & VSRAM[$4E] for both planes in H40 mode, and 0 in H32 mode
			vscroll = _latched.H40 ? (readVsram(38) & readVsram(39)) : 0;
		} else if(column < columns) {
			vscroll = readVsram((uint8_t)(column * 2 + plane));
		} else {
			vscroll = 0;
		}
	}

	return vscroll & (_latched.InterlaceMode == 3 ? 0x7FF : 0x3FF);
}

void MdVdp::RenderBackgrounds(uint16_t line, int16_t startCol, int16_t endCol, int16_t pixelOffset)
{
	bool im2 = _latched.InterlaceMode == 3;
	uint8_t cellShift = im2 ? 4 : 3;

	auto sizeToPixels = [](uint8_t size) -> uint16_t {
		switch(size) {
			default:
			case 0: return 32 * 8;
			case 1: return 64 * 8;
			case 2: return 32 * 8;
			case 3: return 128 * 8;
		}
	};

	uint16_t hSizePixels;
	uint16_t vSizePixels;
	if(_latched.HScrollSize == 2) {
		//An invalid H size always produces a 32x1 plane
		hSizePixels = 32 * 8;
		vSizePixels = 8;
	} else if(_latched.VScrollSize == 2) {
		//An invalid V size behaves like a size of 32 (when the H size is valid)
		hSizePixels = sizeToPixels(_latched.HScrollSize);
		vSizePixels = 32 * 8;
	} else {
		hSizePixels = sizeToPixels(_latched.HScrollSize);
		vSizePixels = sizeToPixels(_latched.VScrollSize);
	}

	uint16_t hSizeCells = hSizePixels / 8;
	uint16_t nametableWidth = _latched.HScrollSize == 2 ? 1 : hSizeCells;
	uint16_t lineMask = im2 ? ((vSizePixels << 1) - 1) : (vSizePixels - 1);
	int16_t activeCells = (int16_t)(GetActivePixels(_latched) / 8);

	for(uint8_t plane = 0; plane < 2; plane++) {
		bool planeA = plane == 0;
		TilePixel* buffer = planeA ? _planeAPixels : _planeBPixels;
		if(planeA ? _disablePlaneA : _disablePlaneB) {
			memset(buffer, 0, sizeof(TilePixel) * MdVdp::BgBufferWidth);
			continue;
		}

		uint16_t ntBase = planeA ? _latched.PlaneATable : _latched.PlaneBTable;
		uint16_t hscroll = _latchedHScroll[plane];
		uint16_t coarseScroll = hscroll & ~0x0F;
		int16_t fineScroll = hscroll & 0x0F;

		//The VDP always renders a column partially or fully to the left of the active display (column -1)
		int16_t startColumn = (fineScroll != 0 || _showHorizontalBorder) ? -1 : 0;
		int16_t endColumn = activeCells / 2;

		for(int16_t column = startColumn; column < endColumn; column++) {
			uint16_t vscroll = ReadVScroll(plane, column);
			uint16_t scrolledLine = (line + vscroll) & lineMask;
			uint16_t vCell = scrolledLine >> cellShift;
			uint16_t columnCell = (uint16_t)((uint16_t)(16 * column) - coarseScroll) / 8;

			for(int16_t cellOffset = 0; cellOffset < 2; cellOffset++) {
				int16_t cellCol = 16 * column + 8 * cellOffset + fineScroll + pixelOffset;
				if(cellCol + 8 <= startCol || cellCol >= endCol) {
					continue;
				}

				uint16_t hCell = (columnCell + cellOffset) & (hSizeCells - 1);

				//Nametables are limited to 8KB
				uint16_t ntAddr = (ntBase + ((2 * (vCell * nametableWidth + hCell)) & 0x1FFF)) & 0xFFFF;
				uint16_t entry = (_videoRam[ntAddr] << 8) | _videoRam[(ntAddr + 1) & 0xFFFF];
				uint8_t palette = (entry >> 13) & 0x03;
				bool priority = entry & 0x8000;
				bool vflip = entry & 0x1000;
				bool hflip = entry & 0x0800;

				if(!planeA) {
					_lastScrollBPalettes[0] = _lastScrollBPalettes[1];
					_lastScrollBPalettes[1] = palette;
				}

				uint16_t cellHeight = 1 << cellShift;
				uint16_t cellRow = vflip ? (cellHeight - 1 - (scrolledLine & (cellHeight - 1))) : (scrolledLine & (cellHeight - 1));
				uint16_t rowAddr = (uint16_t)((4 * cellHeight) * (entry & 0x7FF) + 4 * cellRow);

				for(int16_t i = 0; i < 8; i++) {
					int16_t col = cellCol + i;
					if(col < startCol || col >= endCol) {
						continue;
					}

					uint8_t px = hflip ? (7 - i) : i;
					uint8_t data = _videoRam[(rowAddr + (px >> 1)) & 0xFFFF];
					buffer[col] = { (uint8_t)((px & 0x01) ? (data & 0x0F) : (data >> 4)), palette, priority };
				}
			}
		}
	}
}

void MdVdp::RenderWindow(uint16_t line, int16_t startCol, int16_t endCol, int16_t pixelOffset)
{
	if(_disableWindow) {
		return;
	}

	uint16_t activePixels = GetActivePixels(_latched);
	uint16_t windowStart;
	uint16_t windowEnd;
	bool inWindowRows = _latched.WindowDown ? (line / 8 >= _latched.WindowY) : (line / 8 < _latched.WindowY);
	if(inWindowRows) {
		windowStart = 0;
		windowEnd = activePixels;
	} else if(_latched.WindowRight) {
		windowStart = std::min<uint16_t>(8 * _latched.WindowX, activePixels);
		windowEnd = activePixels;
	} else {
		windowStart = 0;
		windowEnd = std::min<uint16_t>(8 * _latched.WindowX, activePixels);
	}

	if(windowStart >= windowEnd) {
		return;
	}

	uint8_t cellShift = _latched.InterlaceMode == 3 ? 4 : 3;
	uint16_t cellHeight = 1 << cellShift;
	uint16_t widthCells = _latched.H40 ? 64 : 32;
	uint16_t vCell = line >> cellShift;

	for(uint16_t hCell = windowStart / 8; hCell < windowEnd / 8; hCell++) {
		int16_t cellCol = (int16_t)(8 * hCell) + pixelOffset;
		if(cellCol + 8 <= startCol || cellCol >= endCol) {
			continue;
		}

		uint16_t ntAddr = (_latched.WindowTable + ((2 * (vCell * widthCells + hCell)) & 0x1FFF)) & 0xFFFF;
		uint16_t entry = (_videoRam[ntAddr] << 8) | _videoRam[(ntAddr + 1) & 0xFFFF];
		uint8_t palette = (entry >> 13) & 0x03;
		bool priority = entry & 0x8000;
		bool vflip = entry & 0x1000;
		bool hflip = entry & 0x0800;
		uint16_t cellRow = vflip ? (cellHeight - 1 - (line & (cellHeight - 1))) : (line & (cellHeight - 1));
		uint16_t rowAddr = (uint16_t)((4 * cellHeight) * (entry & 0x7FF) + 4 * cellRow);

		for(int16_t i = 0; i < 8; i++) {
			int16_t col = cellCol + i;
			if(col < startCol || col >= endCol) {
				continue;
			}

			//The window replaces plane A
			uint8_t px = hflip ? (7 - i) : i;
			uint8_t data = _videoRam[(rowAddr + (px >> 1)) & 0xFFFF];
			_planeAPixels[col] = { (uint8_t)((px & 0x01) ? (data & 0x0F) : (data >> 4)), palette, priority };
		}
	}
}

void MdVdp::MergeLayers(RasterLine line, uint32_t row, uint32_t startCol, uint32_t endCol, int16_t pixelOffset, bool oddLine)
{
	SpriteBuffers& sprites = oddLine ? _spritesOdd : _sprites;
	uint8_t backdrop = GetBackdropIndex();
	int16_t activePixels = (int16_t)GetActivePixels(_latched);
	bool shadowHighlight = _latched.ShadowHighlight;
	bool inVBorder = line.InVBorder && !_vBorderForgotten;

	for(uint32_t col = startCol; col < endCol; col++) {
		int16_t pixel = (int16_t)col - pixelOffset;
		TilePixel s = {};
		if(!_disableSprites && pixel >= 0 && pixel < 320) {
			s = sprites.Pixels[pixel];
		}
		TilePixel a = _planeAPixels[col];
		TilePixel b = _planeBPixels[col];

		uint8_t sIndex = (s.Palette << 4) | s.Color;
		uint8_t aIndex = (a.Palette << 4) | a.Color;
		uint8_t bIndex = (b.Palette << 4) | b.Color;

		bool inHBorder = pixel < 0 || pixel >= activePixels;
		if(inHBorder || inVBorder || _debugDisplayDisabled) {
			//Outside of the active display (or when the debug register disables the display), the forced plane is shown as-is
			uint8_t index;
			switch(_forcedPlane) {
				default:
				case ForcedPlane::Backdrop: index = backdrop; break;
				case ForcedPlane::Sprite: index = inHBorder ? 0 : sIndex; break;
				case ForcedPlane::PlaneA: index = aIndex; break;
				case ForcedPlane::PlaneB: index = bIndex; break;
			}
			SetPixel(row, col, index);
			continue;
		}

		if(_latched.LeftColumnBlank && pixel < 8) {
			//Register #0 bit 5 blanks the left-most 8 pixels
			SetPixel(row, col, backdrop);
			continue;
		}

		PixelMode mode = (shadowHighlight && !a.Priority && !b.Priority) ? PixelMode::Shadow : PixelMode::Normal;

		//Order the layers by priority (front-most first)
		const TilePixel* layers[3];
		bool isSprite[3];
		auto setOrder = [&](const TilePixel* l0, bool s0, const TilePixel* l1, bool s1, const TilePixel* l2, bool s2) {
			layers[0] = l0; isSprite[0] = s0;
			layers[1] = l1; isSprite[1] = s1;
			layers[2] = l2; isSprite[2] = s2;
		};

		if(s.Priority || (!a.Priority && !b.Priority)) {
			if(!s.Priority || a.Priority || !b.Priority) {
				setOrder(&s, true, &a, false, &b, false);
			} else {
				setOrder(&s, true, &b, false, &a, false);
			}
		} else if(a.Priority && !b.Priority) {
			setOrder(&a, false, &s, true, &b, false);
		} else if(!a.Priority && b.Priority) {
			setOrder(&b, false, &s, true, &a, false);
		} else {
			setOrder(&a, false, &b, false, &s, true);
		}

		uint8_t maskIndex;
		switch(_forcedPlane) {
			default:
			case ForcedPlane::Backdrop: maskIndex = 0x3F; break;
			case ForcedPlane::Sprite: maskIndex = sIndex; break;
			case ForcedPlane::PlaneA: maskIndex = aIndex; break;
			case ForcedPlane::PlaneB: maskIndex = bIndex; break;
		}

		bool found = false;
		for(int i = 0; i < 3; i++) {
			const TilePixel* layer = layers[i];
			if(layer->Color == 0) {
				continue;
			}

			if(shadowHighlight && isSprite[i] && layer->Palette == 3) {
				if(layer->Color == 14) {
					//Highlight operator - the sprite is transparent and the pixel below it is highlighted
					mode = mode == PixelMode::Shadow ? PixelMode::Normal : PixelMode::Highlight;
					continue;
				} else if(layer->Color == 15) {
					//Shadow operator
					mode = PixelMode::Shadow;
					continue;
				}
			}

			//When the debug register forces a plane, its color index masks the color of the front-most pixel (Overdrive 2 uses this)
			uint8_t index = ((layer->Palette << 4) | layer->Color) & maskIndex;

			//Sprite color 14 is never shadowed/highlighted, and neither are sprites with the priority bit set
			if(shadowHighlight && isSprite[i] && (layer->Color == 14 || s.Priority)) {
				mode = PixelMode::Normal;
			}

			SetPixel(row, col, index, mode);
			found = true;
			break;
		}

		if(!found) {
			uint8_t index;
			switch(_forcedPlane) {
				default:
				case ForcedPlane::Backdrop: index = backdrop; break;
				case ForcedPlane::Sprite: index = sIndex; break;
				case ForcedPlane::PlaneA: index = aIndex; break;
				case ForcedPlane::PlaneB: index = bIndex; break;
			}
			SetPixel(row, col, index, mode);
		}
	}
}

void MdVdp::RenderVerticalBorderLine(uint16_t scanline, uint32_t row, uint16_t startPixel)
{
	switch(_forcedPlane) {
		case ForcedPlane::Backdrop:
			FillRow(row, startPixel, GetBackdropIndex());
			break;

		case ForcedPlane::Sprite:
			FillRow(row, startPixel, 0);
			break;

		case ForcedPlane::PlaneA:
		case ForcedPlane::PlaneB: {
			//The VRAM chip keeps cycling through the 256 bytes in the VRAM row of the last address that was fetched during rendering
			//(the 4th sprite tile fetched for the line after the active display) - the VDP shows these bytes as pixels, using
			//the last palettes used by plane B. A VRAM row contains 64 4-byte groups, each 1KB apart in VRAM's address space.
			bool h40 = _regs.H40;
			uint32_t width = GetCurrentScreenWidth();
			uint32_t startCol;
			uint32_t endPixel;
			if(_showHorizontalBorder) {
				startCol = 0;
				endPixel = width;
			} else {
				startCol = GetLeftBorder(h40);
				endPixel = startCol + (h40 ? 320 : 256);
			}

			uint16_t groupOffset = (uint16_t)((uint16_t)(scanline + 2 - GetActiveScanlines(_regs)) * ((h40 ? 420 : 342) / 2));
			uint16_t addr = (uint16_t)(_sprites.LastTileAddresses[3] + groupOffset * 1024);
			bool oddGroup = false;
			uint8_t group[4] = {};

			for(uint32_t pixel = 0; pixel < endPixel; pixel++) {
				uint32_t tileCol = (pixel + 3) % 8;
				if(pixel == 0 || tileCol == 0) {
					for(int i = 0; i < 4; i++) {
						group[i] = _videoRam[(uint16_t)(addr + i)];
					}
					addr += oddGroup ? 1024 : 7 * 1024;
					oddGroup = !oddGroup;
				}

				if(pixel < startCol) {
					continue;
				}

				uint8_t palette = _lastScrollBPalettes[(pixel / 8) & 0x01];
				uint8_t data = group[tileCol >> 1];
				uint8_t color = (tileCol & 0x01) ? (data & 0x0F) : (data >> 4);
				SetPixel(row, pixel - startCol, (palette << 4) | color);
			}
			break;
		}
	}
}

void MdVdp::RenderLeftBorder(uint32_t row)
{
	uint32_t leftBorder = GetLeftBorder(_latched.H40);

	switch(_forcedPlane) {
		case ForcedPlane::Backdrop:
			for(uint32_t col = 0; col < leftBorder; col++) {
				SetPixel(row, col, GetBackdropIndex());
			}
			break;

		case ForcedPlane::Sprite:
			for(uint32_t col = 0; col < leftBorder; col++) {
				SetPixel(row, col, 0);
			}
			break;

		case ForcedPlane::PlaneA: {
			//The hardware shows unspecified garbage here - use color 0
			uint32_t borderPixels = _latchedHScroll[0] & 0x0F;
			uint32_t borderOffset = 16 - leftBorder;
			uint32_t endCol = borderPixels > borderOffset ? borderPixels - borderOffset : 0;
			for(uint32_t col = 0; col < endCol; col++) {
				SetPixel(row, col, 0);
			}
			break;
		}

		case ForcedPlane::PlaneB: {
			//Pixels from sprite tiles 36 and 37, using the palettes from the last 2 tiles of plane B on the previous line
			uint32_t borderPixels = _latchedHScroll[1] & 0x0F;
			uint32_t borderOffset = 16 - leftBorder;
			uint32_t endCol = borderPixels > borderOffset ? borderPixels - borderOffset : 0;
			for(uint32_t col = 0; col < endCol; col++) {
				uint16_t pixel = (uint16_t)(15 - (endCol - 1 - col));
				RenderHorizontalBorderSpritePixel(row, col, pixel, 36);
			}
			break;
		}
	}
}

void MdVdp::RenderRightBorder(uint32_t row)
{
	uint32_t width = GetCurrentScreenWidth();
	uint32_t borderStart = width - MdVdp::RightBorder;

	switch(_forcedPlane) {
		case ForcedPlane::Backdrop:
			for(uint32_t col = borderStart; col < width; col++) {
				SetPixel(row, col, GetBackdropIndex());
			}
			break;

		case ForcedPlane::Sprite:
			for(uint32_t col = borderStart; col < width; col++) {
				SetPixel(row, col, 0);
			}
			break;

		case ForcedPlane::PlaneA:
		case ForcedPlane::PlaneB: {
			//Pixels from sprite tiles 0-1 (plane A) or 4-5 (plane B), using the palettes from the last 2 tiles of plane B on the previous line
			bool planeA = _forcedPlane == ForcedPlane::PlaneA;
			uint32_t lastColumnEnd = borderStart + std::min<uint32_t>(_latchedHScroll[planeA ? 0 : 1] & 0x0F, MdVdp::RightBorder);
			for(uint32_t col = lastColumnEnd; col < width; col++) {
				RenderHorizontalBorderSpritePixel(row, col, (uint16_t)(col - lastColumnEnd), planeA ? 0 : 4);
			}
			break;
		}
	}
}

void MdVdp::RenderHorizontalBorderSpritePixel(uint32_t row, uint32_t col, uint16_t pixel, uint16_t baseSprite)
{
	uint16_t tile = baseSprite + pixel / 8;
	uint16_t tileCol = pixel % 8;
	uint16_t addr = (uint16_t)(_sprites.LastTileAddresses[tile % 40] + (tileCol >> 1));
	uint8_t data = _videoRam[addr];
	uint8_t color = (tileCol & 0x01) ? (data & 0x0F) : (data >> 4);
	uint8_t palette = _lastScrollBPalettes[(pixel / 8) & 0x01];
	SetPixel(row, col, (palette << 4) | color);
}

void MdVdp::ScanSprites(uint16_t scanline)
{
	RasterLine line = GetRasterLine(scanline);

	//In the vertical border, sprites are only scanned for the line immediately after the active display
	if(line.InVBorder && !_vBorderForgotten && scanline != GetActiveScanlines(_regs)) {
		return;
	}

	if(_latched.InterlaceMode == 3 && _interlacedFrame) {
		DoSpriteScan({ (uint16_t)((line.Line * 2) & 0x1FF), line.InVBorder }, _sprites);
		DoSpriteScan({ (uint16_t)((line.Line * 2 + 1) & 0x1FF), line.InVBorder }, _spritesOdd);
	} else {
		DoSpriteScan(line, _sprites);
		_spritesOdd.ScannedCount = 0;
	}
}

void MdVdp::DoSpriteScan(RasterLine line, SpriteBuffers& buffers)
{
	buffers.ScannedCount = 0;

	bool h40 = _latched.H40;
	uint32_t tableLength = h40 ? 80 : 64;

	//Disabling the display during part of HBlank reduces the number of sprites scanned (approximation)
	uint32_t maxToScan = tableLength > _pixelsDisabledDuringHblank ? tableLength - _pixelsDisabledDuringHblank : 0;

	bool im2 = _latched.InterlaceMode == 3 && _interlacedFrame;
	uint16_t displayMask = im2 ? 0x3FF : 0x1FF;
	uint16_t spriteLine = ((im2 ? 0x100 : 0x80) + line.Line) & displayMask;
	uint16_t cellHeight = im2 ? 16 : 8;
	uint8_t maxPerLine = h40 ? 20 : 16;

	uint32_t index = 0;
	for(uint32_t i = 0; i < maxToScan; i++) {
		uint8_t* cache = &_latchedSpriteCache[index * 4];
		uint16_t top = ((cache[0] << 8) | cache[1]) & displayMask;
		uint16_t bottom = top + cellHeight * ((cache[2] & 0x03) + 1);
		if(spriteLine >= top && spriteLine < bottom) {
			if(buffers.ScannedCount >= maxPerLine) {
				_spriteOverflow = true;
				if(!_removeSpriteLimit) {
					break;
				}
			}
			if(buffers.ScannedCount < 80) {
				buffers.ScannedIds[buffers.ScannedCount++] = (uint8_t)index;
			}
		}

		index = cache[3] & 0x7F;
		if(index == 0 || index >= tableLength) {
			break;
		}
	}
}

void MdVdp::FetchSpriteAttributes()
{
	DoSpriteAttributeFetch(_sprites);
	if(_latched.InterlaceMode == 3 && _interlacedFrame) {
		DoSpriteAttributeFetch(_spritesOdd);
	} else {
		_spritesOdd.SpriteCount = 0;
	}
}

void MdVdp::DoSpriteAttributeFetch(SpriteBuffers& buffers)
{
	buffers.SpriteCount = 0;
	uint16_t table = (uint16_t)(_regs.SpriteTable & 0xFFFF);

	for(uint8_t i = 0; i < buffers.ScannedCount; i++) {
		uint8_t id = buffers.ScannedIds[i];
		uint8_t* cache = &_spriteCache[id * 4];
		uint16_t addr = (uint16_t)(table + 8 * id);
		uint8_t b0 = _videoRam[(uint16_t)(addr + 4)];
		uint8_t b1 = _videoRam[(uint16_t)(addr + 5)];
		uint8_t b2 = _videoRam[(uint16_t)(addr + 6)];
		uint8_t b3 = _videoRam[(uint16_t)(addr + 7)];

		SpriteInfo& sprite = buffers.Sprites[buffers.SpriteCount++];
		sprite.Y = (cache[0] << 8) | cache[1];
		sprite.HeightCells = (cache[2] & 0x03) + 1;
		sprite.WidthCells = ((cache[2] >> 2) & 0x03) + 1;
		sprite.Priority = b0 & 0x80;
		sprite.Palette = (b0 >> 5) & 0x03;
		sprite.VFlip = b0 & 0x10;
		sprite.HFlip = b0 & 0x08;
		sprite.Pattern = ((b0 & 0x07) << 8) | b1;
		sprite.X = ((b2 & 0x01) << 8) | b3;
	}
}

void MdVdp::RenderSpritePixels(RasterLine line, SpriteBuffers& buffers)
{
	memset(buffers.Pixels, 0, sizeof(buffers.Pixels));

	bool h40 = _latched.H40;
	uint16_t activePixels = h40 ? 320 : 256;
	bool im2 = _latched.InterlaceMode == 3 && _interlacedFrame;
	uint16_t displayMask = im2 ? 0x3FF : 0x1FF;
	uint16_t spriteLine = (im2 ? 0x100 : 0x80) + line.Line;
	uint8_t cellShift = im2 ? 4 : 3;

	//Disabling the display during HBlank also reduces the number of sprite tiles that can be fetched (approximation)
	uint16_t tilesSkipped = (_pixelsDisabledDuringHblank + 1) / 2;
	uint16_t maxTiles = activePixels / 8;
	maxTiles = maxTiles > tilesSkipped ? maxTiles - tilesSkipped : 0;

	uint16_t tilesFetched = 0;
	bool dotOverflow = false;

	//A sprite at X=0 masks all lower priority sprites on the line, if a sprite with X!=0 was found before it (or the previous line had a dot overflow)
	bool foundNonZero = _dotOverflowOnPrevLine;
	bool maskSprites = false;

	for(uint8_t i = 0; i < buffers.SpriteCount; i++) {
		SpriteInfo& sprite = buffers.Sprites[i];
		if(sprite.X == 0 && foundNonZero) {
			maskSprites = true;
		} else if(sprite.X != 0) {
			foundNonZero = true;
		}

		//Only the lowest 5 bits of the difference between the line and the sprite's Y position are used - the Y position
		//can change between the sprite scan and this point (Overdrive 2's textured cube depends on this)
		uint16_t spriteRow = (uint16_t)(spriteLine - (sprite.Y & displayMask)) & 0x1F;
		if(sprite.VFlip) {
			spriteRow = (uint16_t)(((sprite.HeightCells << cellShift) - 1) - spriteRow) & 0x1F;
		}

		bool limitReached = false;
		for(uint8_t cell = 0; cell < sprite.WidthCells; cell++) {
			if(tilesFetched >= maxTiles) {
				_spriteOverflow = true;
				dotOverflow = true;
				if(!_removeSpriteLimit) {
					limitReached = true;
					break;
				}
			}

			uint8_t cellCol = sprite.HFlip ? (sprite.WidthCells - 1 - cell) : cell;
			uint16_t pattern = (uint16_t)(sprite.Pattern + cellCol * sprite.HeightCells + (spriteRow >> cellShift));
			uint16_t cellAddr = (uint16_t)((4 << cellShift) * pattern);
			uint16_t cellRow = spriteRow & ((1 << cellShift) - 1);
			uint16_t rowAddr = (uint16_t)(cellAddr + 4 * cellRow);

			//The addresses fetched are needed to render the borders (Overdrive 2)
			if(tilesFetched < maxTiles && tilesFetched < 40) {
				buffers.LastTileAddresses[tilesFetched] = rowAddr;
			}
			tilesFetched++;

			if(maskSprites) {
				continue;
			}

			uint16_t cellX = sprite.X + 8 * cell;
			if(cellX >= 0x80 + activePixels || cellX + 8 <= 0x80) {
				continue;
			}

			for(uint8_t px = 0; px < 8; px++) {
				uint16_t x = cellX + px;
				if(x < 0x80 || x >= 0x80 + activePixels) {
					continue;
				}

				uint8_t srcPx = sprite.HFlip ? (7 - px) : px;
				uint8_t data = _videoRam[(uint16_t)(rowAddr + (srcPx >> 1))];
				uint8_t color = (srcPx & 0x01) ? (data & 0x0F) : (data >> 4);

				TilePixel& dst = buffers.Pixels[x - 0x80];
				if(dst.Color == 0) {
					//Transparent pixels are always overwritten, even by transparent pixels (Overdrive 2's title screen depends on this)
					dst = { color, sprite.Palette, sprite.Priority };
				} else if(color != 0) {
					_spriteCollision = true;
				}
			}
		}

		if(limitReached) {
			break;
		}
	}

	_dotOverflowOnPrevLine = dotOverflow;
}

MdVdpState& MdVdp::GetState()
{
	uint8_t* r = _state.Regs;
	bool vblank;
	_state.VCounter = GetVCounter(_state.HClock, vblank);
	_state.HCounter = PixelToInternalH(MclkToPixel(_state.HClock, _regs.H40), _regs.H40);

	_state.AddressReg = _dataAddress;
	_state.CodeReg = (_isWriteMode ? 0x01 : 0) | (_locationBits << 1) | (_dmaActive ? 0x20 : 0);
	_state.ControlPending = _controlPending;

	_state.FifoCount = _fifoCount;
	_state.FifoReadPos = _fifoPopIndex;
	for(int i = 0; i < 4; i++) {
		_state.Fifo[i].Address = _fifo[i].Address;
		_state.Fifo[i].Value = _fifo[i].Value;
		_state.Fifo[i].Code = (uint8_t)_fifo[i].Location;
		_state.Fifo[i].HalfWritten = _fifo[i].SecondHalf;
	}

	_state.VerticalIrqPending = _vIntPending;
	_state.HorizontalIrqPending = _hIntPending;
	_state.SpriteOverflow = _spriteOverflow;
	_state.SpriteCollision = _spriteCollision;
	_state.OddFrame = _interlacedOdd;
	_state.InVerticalBlank = _inVblank;
	_state.InHorizontalBlank = (GetStatus() & 0x04) != 0;
	_state.HIntCounter = (uint8_t)_hIntCounter;
	_state.HvLatch = _hvLatch;
	_state.HvLatched = _hvLatched;

	_state.DmaActive = _dmaActive;
	_state.DmaFillPending = _dmaActive && _state.DmaMode == MdVdpDmaMode::VramFill && !_fillDataReady;
	_state.DmaFillValue = _fillData;
	_state.DmaLength = _dmaLength;
	_state.DmaSource = _dmaSource;

	_state.DisplayEnabled = _regs.DisplayEnabled;
	_state.H40Mode = _regs.H40;
	_state.V30Mode = _regs.V30;
	_state.Mode5 = _regs.Mode5;
	_state.ShadowHighlightEnabled = _regs.ShadowHighlight;
	_state.InterlaceMode = (r[12] >> 1) & 0x03;
	_state.EnableHorizontalIrq = _regs.HIntEnabled;
	_state.EnableVerticalIrq = _regs.VIntEnabled;
	_state.EnableDma = _regs.DmaEnabled;
	_state.EnableExternalIrq = r[11] & 0x08;
	_state.HvCounterLatchEnabled = _regs.HvCounterStopped;
	_state.MaskLeftColumn = _regs.LeftColumnBlank;
	_state.LeftColumnBlank = _regs.LeftColumnBlank;
	_state.AddressIncrement = _regs.AutoIncrement;
	_state.BackgroundColorIndex = _regs.BackdropColor;
	_state.ExtVideoEnabled = r[0] & 0x01;

	_state.PlaneATableAddress = _regs.PlaneATable;
	_state.PlaneBTableAddress = _regs.PlaneBTable;
	_state.WindowTableAddress = _regs.WindowTable;
	_state.SpriteTableAddress = (uint16_t)_regs.SpriteTable;
	_state.HScrollTableAddress = _regs.HScrollTable;

	constexpr uint8_t planeSizes[4] = { 32, 64, 32, 128 };
	_state.PlaneWidth = planeSizes[_regs.HScrollSize];
	_state.PlaneHeight = planeSizes[_regs.VScrollSize];
	_state.HScrollMode = _regs.HScrollMode;
	_state.VScrollColumnMode = _regs.VScroll2Cell;

	_state.WindowHPos = _regs.WindowX >> 1;
	_state.WindowRight = _regs.WindowRight;
	_state.WindowVPos = _regs.WindowY;
	_state.WindowDown = _regs.WindowDown;

	return _state;
}

void MdVdp::DebugSendFrame()
{
	RenderedFrame frame(_currentOutputBuffer, GetCurrentScreenWidth(), GetCurrentScreenHeight(), 1.0, _state.FrameCount);
	_emu->GetVideoDecoder()->UpdateFrame(frame, false, false);
}

void MdVdp::Serialize(Serializer& s)
{
	SV(_state.FrameCount);
	SV(_state.HClock);
	SV(_state.Scanline);
	SVArray(_state.Regs, 0x20);
	SV(_state.DmaMode);

	SVArray(_videoRam, MdConstants::VideoRamSize);
	SVArray(_colorRam, MdConstants::ColorRamSize);
	SVArray(_vsRam, MdConstants::VsRamSize);
	SVArray(_spriteCache, 80 * 4);
	SVArray(_latchedSpriteCache, 80 * 4);

	SV(_lastMasterClock);
	SVArray(_latchedHScroll, 2);
	SVArray(_latchedVScroll, 2);
	SV(_debugDisplayDisabled);
	SV(_forcedPlane);

	SV(_pixel);
	SV(_eventIndex);
	SV(_inVblank);
	SV(_vBorderForgotten);
	SV(_topBorder);
	SV(_frameH40);
	SV(_interlacedFrame);
	SV(_interlacedOdd);
	SVArray(_lastScrollBPalettes, 2);

	SV(_dmaActive);
	SV(_isWriteMode);
	SV(_locationBits);
	SV(_location);
	SV(_controlAddress);
	SV(_dataAddress);
	SV(_controlPending);

	for(int i = 0; i < 4; i++) {
		SVI(_fifo[i].Address);
		SVI(_fifo[i].Value);
		SVI(_fifo[i].Location);
		SVI(_fifo[i].IsWrite);
		SVI(_fifo[i].ByteWrite);
		SVI(_fifo[i].SecondHalf);
		SVI(_fifo[i].Latency);
	}
	SV(_fifoCount);
	SV(_fifoPopIndex);
	SV(_fifoPushIndex);
	SV(_dmaLatency);
	SV(_fillDataReady);
	SV(_fillData);
	SV(_copyOddSlot);
	SV(_displayEnablePending);
	SV(_dmaSource);
	SV(_dmaLength);

	SV(_vIntPending);
	SV(_hIntPending);
	SV(_hIntCounter);
	SV(_z80IntActive);
	SV(_hvLatch);
	SV(_hvLatched);

	SV(_spriteOverflow);
	SV(_spriteCollision);
	SV(_dotOverflowOnPrevLine);
	SV(_pixelsDisabledDuringHblank);
	SV(_spriteDisplayEnabled);
	SV(_displayEnabledPixel);

	SpriteBuffers* buffers[2] = { &_sprites, &_spritesOdd };
	for(int b = 0; b < 2; b++) {
		SpriteBuffers& buf = *buffers[b];
		SVArray(buf.ScannedIds, 80);
		SV(buf.ScannedCount);
		SV(buf.SpriteCount);
		SVArray(buf.LastTileAddresses, 40);
		for(int i = 0; i < 80; i++) {
			SVI(buf.Sprites[i].Pattern);
			SVI(buf.Sprites[i].Y);
			SVI(buf.Sprites[i].X);
			SVI(buf.Sprites[i].WidthCells);
			SVI(buf.Sprites[i].HeightCells);
			SVI(buf.Sprites[i].Palette);
			SVI(buf.Sprites[i].VFlip);
			SVI(buf.Sprites[i].HFlip);
			SVI(buf.Sprites[i].Priority);
		}
		for(int i = 0; i < 320; i++) {
			SVI(buf.Pixels[i].Color);
			SVI(buf.Pixels[i].Palette);
			SVI(buf.Pixels[i].Priority);
		}
	}

	if(!s.IsSaving()) {
		DecodeRegs(_state.Regs, _regs);
		if(_displayEnablePending) {
			_regs.DisplayEnabled = false;
		}
		_latched = _regs;
		UpdateEventTimes();
		RefreshPalette();
	}
}
