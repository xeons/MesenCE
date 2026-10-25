#pragma once
#include "pch.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/BaseEventManager.h"
#include "MD/MdTypes.h"
#include "Utilities/SimpleLock.h"

enum class DebugEventType;
struct DebugEventInfo;
class MdVdp;
class MdConsole;
class Debugger;

struct MdEventViewerConfig : public BaseEventViewerConfig
{
	EventViewerCategoryCfg Irq;
	EventViewerCategoryCfg MarkedBreakpoints;

	EventViewerCategoryCfg VdpDataWrite;
	EventViewerCategoryCfg VdpDataRead;
	EventViewerCategoryCfg VdpControlWrite;
	EventViewerCategoryCfg VdpControlRead;
	EventViewerCategoryCfg VdpHvCounterRead;

	EventViewerCategoryCfg PsgWrite;
	EventViewerCategoryCfg FmWrite;
	EventViewerCategoryCfg FmRead;

	EventViewerCategoryCfg Z80RamWrite;
	EventViewerCategoryCfg Z80RamRead;
	EventViewerCategoryCfg Z80BankWrite;
	EventViewerCategoryCfg Z80BusControl;

	EventViewerCategoryCfg IoWrite;
	EventViewerCategoryCfg IoRead;
	EventViewerCategoryCfg CartRegisterWrite;

	bool ShowPreviousFrameEvents;
	bool ShowZ80Events;
};

class MdEventManager final : public BaseEventManager
{
private:
	//Each column represents 4 master clocks (3420 per scanline), with some extra room for the end of the active display
	static constexpr int ClocksPerColumn = 4;
	static constexpr int ScanlineWidth = 864;

	MdEventViewerConfig _config = {};

	MdVdp* _vdp = nullptr;
	MdConsole* _console = nullptr;
	Debugger* _debugger = nullptr;

	uint32_t _scanlineCount = MdConstants::NtscScanlineCount;
	uint16_t* _ppuBuffer = nullptr;
	uint16_t _rowWidths[MdConstants::MaxScreenHeight] = {};
	uint32_t _frameHeight = 224;
	bool _interlaceMode2 = false;
	uint16_t _activeStartColumn = 0;

	void SetVdpTarget(DebugEventInfo& evt, uint8_t port);

protected:
	bool ShowPreviousFrameEvents() override;
	void ConvertScanlineCycleToRowColumn(int32_t& x, int32_t& y) override;
	void DrawScreen(uint32_t* buffer) override;

public:
	MdEventManager(Debugger* debugger, MdConsole* console);
	~MdEventManager();

	void AddEvent(DebugEventType type, MemoryOperationInfo& operation, int32_t breakpointId = -1) override;
	void AddEvent(DebugEventType type) override;

	EventViewerCategoryCfg GetEventConfig(DebugEventInfo& evt) override;

	uint32_t TakeEventSnapshot(bool forAutoRefresh) override;
	DebugEventInfo GetEvent(uint16_t y, uint16_t x) override;

	FrameInfo GetDisplayBufferSize() override;
	void SetConfiguration(BaseEventViewerConfig& config) override;
};
