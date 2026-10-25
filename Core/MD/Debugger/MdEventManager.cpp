#include "pch.h"
#include "MD/Debugger/MdEventManager.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/MdVdp.h"
#include "MD/MdConsole.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/BaseEventManager.h"

MdEventManager::MdEventManager(Debugger* debugger, MdConsole* console)
{
	_debugger = debugger;
	_console = console;
	_vdp = console->GetVdp();

	_ppuBuffer = new uint16_t[MdConstants::PixelCount];
	memset(_ppuBuffer, 0, MdConstants::PixelCount * sizeof(uint16_t));
}

MdEventManager::~MdEventManager()
{
	delete[] _ppuBuffer;
}

void MdEventManager::SetVdpTarget(DebugEventInfo& evt, uint8_t port)
{
	MdVdpState& state = _vdp->GetState();
	if(evt.Operation.Type != MemoryOperationType::Write) {
		return;
	}

	//Byte writes are written to both halves of the bus
	uint16_t value = evt.Operation.Value > 0xFF ? (uint16_t)evt.Operation.Value : (uint16_t)((evt.Operation.Value << 8) | evt.Operation.Value);

	if(port < 0x04) {
		evt.TargetMemory.Type = MemoryOperationType::Write;
		evt.TargetMemory.Value = value;
		switch(state.CodeReg & 0x0F) {
			case 0x01:
				evt.Flags |= (uint32_t)EventFlags::WithTargetMemory;
				evt.TargetMemory.MemType = MemoryType::MdVideoRam;
				evt.TargetMemory.Address = state.AddressReg & 0xFFFF;
				break;

			case 0x03:
				evt.Flags |= (uint32_t)EventFlags::WithTargetMemory;
				evt.TargetMemory.MemType = MemoryType::MdColorRam;
				evt.TargetMemory.Address = state.AddressReg & 0x7E;
				break;

			case 0x05:
				evt.Flags |= (uint32_t)EventFlags::WithTargetMemory;
				evt.TargetMemory.MemType = MemoryType::MdVsRam;
				evt.TargetMemory.Address = state.AddressReg & 0x7E;
				break;
		}
	} else if(port < 0x08) {
		if(state.ControlPending) {
			evt.Flags |= (uint32_t)EventFlags::RegSecondWrite;
		} else if((value & 0xC000) == 0x8000) {
			evt.RegisterId = (value >> 8) & 0x1F;
		} else {
			evt.Flags |= (uint32_t)EventFlags::RegFirstWrite;
		}
	}
}

void MdEventManager::AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Flags = (uint32_t)EventFlags::ReadWriteOp;
	evt.Operation = operation;
	evt.Scanline = (int16_t)_vdp->GetScanline();
	evt.Cycle = _vdp->GetHClock();
	evt.BreakpointId = breakpointId;
	evt.DmaChannel = -1;

	bool fromZ80 = operation.MemType == MemoryType::MdZ80Memory;
	if(type == DebugEventType::Register) {
		if(fromZ80) {
			if(operation.Address >= 0x7F00 && operation.Address < 0x7F20) {
				SetVdpTarget(evt, operation.Address & 0x1F);
			}
		} else if(operation.Address >= 0xC00000 && operation.Address < 0xE00000) {
			SetVdpTarget(evt, operation.Address & 0x1F);
		}
	}

	evt.ProgramCounter = _debugger->GetProgramCounter(fromZ80 ? CpuType::MdZ80 : CpuType::Md, true);
	_debugEvents.push_back(evt);
}

void MdEventManager::AddEvent(DebugEventType type)
{
	DebugEventInfo evt = {};
	evt.Type = type;
	evt.Scanline = (int16_t)_vdp->GetScanline();
	evt.Cycle = _vdp->GetHClock();
	evt.BreakpointId = -1;
	evt.DmaChannel = -1;
	evt.ProgramCounter = _debugger->GetProgramCounter(CpuType::Md, true);
	_debugEvents.push_back(evt);
}

DebugEventInfo MdEventManager::GetEvent(uint16_t y, uint16_t x)
{
	auto lock = _lock.AcquireSafe();

	int column = x;
	int scanline = y / 2;

	//Search without including larger background color first
	for(DebugEventInfo& evt : _sentEvents) {
		if(evt.Cycle / MdEventManager::ClocksPerColumn == column && evt.Scanline == scanline) {
			return evt;
		}
	}

	//If no exact match, extend to the background color
	for(int i = (int)_sentEvents.size() - 1; i >= 0; i--) {
		DebugEventInfo& evt = _sentEvents[i];
		if(std::abs((int)evt.Cycle / MdEventManager::ClocksPerColumn - column) <= 2 && std::abs((int)evt.Scanline - scanline) <= 1) {
			return evt;
		}
	}

	DebugEventInfo empty = {};
	empty.ProgramCounter = 0xFFFFFFFF;
	return empty;
}

bool MdEventManager::ShowPreviousFrameEvents()
{
	return _config.ShowPreviousFrameEvents;
}

void MdEventManager::SetConfiguration(BaseEventViewerConfig& config)
{
	_config = (MdEventViewerConfig&)config;
}

EventViewerCategoryCfg MdEventManager::GetEventConfig(DebugEventInfo& evt)
{
	bool fromZ80 = evt.Operation.MemType == MemoryType::MdZ80Memory;
	if(fromZ80 && !_config.ShowZ80Events && evt.Type != DebugEventType::Irq) {
		return {};
	}

	switch(evt.Type) {
		default: return {};
		case DebugEventType::Breakpoint: return _config.MarkedBreakpoints;
		case DebugEventType::Irq: return _config.Irq;
		case DebugEventType::Register: break;
	}

	uint32_t addr = evt.Operation.Address;
	bool isWrite = evt.Operation.Type == MemoryOperationType::Write || evt.Operation.Type == MemoryOperationType::DummyWrite;

	auto getVdpConfig = [&](uint8_t port) -> EventViewerCategoryCfg {
		if(port < 0x04) {
			return isWrite ? _config.VdpDataWrite : _config.VdpDataRead;
		} else if(port < 0x08) {
			return isWrite ? _config.VdpControlWrite : _config.VdpControlRead;
		} else if(port < 0x10) {
			return isWrite ? EventViewerCategoryCfg {} : _config.VdpHvCounterRead;
		} else if(port < 0x18) {
			return isWrite ? _config.PsgWrite : EventViewerCategoryCfg {};
		}
		return {};
	};

	if(fromZ80) {
		if(addr >= 0x4000 && addr < 0x6000) {
			return isWrite ? _config.FmWrite : _config.FmRead;
		} else if(addr >= 0x6000 && addr < 0x6100) {
			return isWrite ? _config.Z80BankWrite : EventViewerCategoryCfg {};
		} else if(addr >= 0x7F00 && addr < 0x7F20) {
			return getVdpConfig(addr & 0x1F);
		}
		return {};
	}

	if(addr >= 0xC00000 && addr < 0xE00000) {
		return getVdpConfig(addr & 0x1F);
	} else if(addr >= 0xA00000 && addr < 0xA04000) {
		return isWrite ? _config.Z80RamWrite : _config.Z80RamRead;
	} else if(addr >= 0xA04000 && addr < 0xA06000) {
		return isWrite ? _config.FmWrite : _config.FmRead;
	} else if(addr >= 0xA06000 && addr < 0xA06100) {
		return isWrite ? _config.Z80BankWrite : EventViewerCategoryCfg {};
	} else if(addr >= 0xA10000 && addr < 0xA10020) {
		return isWrite ? _config.IoWrite : _config.IoRead;
	} else if(addr >= 0xA11100 && addr < 0xA11300) {
		return _config.Z80BusControl;
	} else if(addr >= 0xA13000 && addr < 0xA13100) {
		return isWrite ? _config.CartRegisterWrite : EventViewerCategoryCfg {};
	}

	return {};
}

void MdEventManager::ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y)
{
	y *= 2;
	x /= MdEventManager::ClocksPerColumn;
}

uint32_t MdEventManager::TakeEventSnapshot(bool forAutoRefresh)
{
	DebugBreakHelper breakHelper(_debugger);
	auto lock = _lock.AcquireSafe();

	uint16_t cycle = _vdp->GetHClock();
	uint16_t scanline = _vdp->GetScanline();
	uint16_t activeLines = _vdp->GetActiveLineCount();

	_frameHeight = _vdp->GetScreenHeight();
	_interlaceMode2 = _frameHeight > 240;
	uint32_t frameWidth = _vdp->GetScreenWidth();

	//The last completed frame is in the current buffer during vblank, and in the previous buffer during the active display
	bool inActiveDisplay = scanline < activeLines;
	uint16_t* completedFrame = _vdp->GetScreenBuffer(inActiveDisplay);

	uint32_t rowsPerLine = _interlaceMode2 ? 2 : 1;
	uint32_t renderedRows = (inActiveDisplay && !(forAutoRefresh && scanline == 0 && cycle == 0)) ? scanline * rowsPerLine : 0;
	uint16_t* renderBuffer = _vdp->GetRenderBuffer();

	for(uint32_t row = 0; row < _frameHeight && row < MdConstants::MaxScreenHeight; row++) {
		uint16_t* dst = _ppuBuffer + row * MdConstants::MaxScreenWidth;
		if(row < renderedRows) {
			//Use the lines that have already been rendered for the current frame
			_rowWidths[row] = _vdp->GetRenderLineWidth(row);
			memcpy(dst, renderBuffer + row * MdConstants::MaxScreenWidth, _rowWidths[row] * sizeof(uint16_t));
		} else {
			_rowWidths[row] = (uint16_t)frameWidth;
			memcpy(dst, completedFrame + row * frameWidth, frameWidth * sizeof(uint16_t));
		}
	}

	_activeStartColumn = _vdp->GetActiveDisplayStartClock() / MdEventManager::ClocksPerColumn;

	_snapshotCurrentFrame = _debugEvents;
	_snapshotPrevFrame = _prevDebugEvents;
	_snapshotScanline = scanline;
	_snapshotCycle = cycle;
	_forAutoRefresh = forAutoRefresh;
	_scanlineCount = _vdp->GetScanlineCount();
	return _scanlineCount;
}

FrameInfo MdEventManager::GetDisplayBufferSize()
{
	FrameInfo size;
	size.Width = MdEventManager::ScanlineWidth;
	size.Height = _scanlineCount * 2;
	return size;
}

void MdEventManager::DrawScreen(uint32_t* buffer)
{
	//The active display covers 2560 master clocks in both H32 and H40 modes
	constexpr uint32_t screenColumns = 2560 / MdEventManager::ClocksPerColumn;

	uint32_t rowCount = _interlaceMode2 ? _frameHeight : _frameHeight * 2;
	for(uint32_t y = 0; y < rowCount && y < _scanlineCount * 2; y++) {
		uint32_t srcRow = _interlaceMode2 ? y : (y >> 1);
		uint16_t* src = _ppuBuffer + srcRow * MdConstants::MaxScreenWidth;
		uint32_t rowWidth = _rowWidths[srcRow];
		if(rowWidth == 0) {
			continue;
		}

		for(uint32_t x = 0; x < screenColumns; x++) {
			uint32_t column = _activeStartColumn + x;
			if(column >= MdEventManager::ScanlineWidth) {
				break;
			}
			buffer[y * MdEventManager::ScanlineWidth + column] = MdVdpTools::GetRgbColor(src[x * rowWidth / screenColumns]);
		}
	}
}
