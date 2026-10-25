#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/SettingTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdCpu;
class MdZ80Cpu;
class MdMemoryManager;

//Sega 315-5313 VDP (mode 5)
//Timing, border and debug register behavior is based on research by the Titan demo group (Overdrive 2)
//and on jgenesis' implementation (GPLv3, https://github.com/jsgroth/jgenesis)
class MdVdp final : public ISerializable
{
public:
	//Border sizes, in pixels/lines
	static constexpr uint16_t H32LeftBorder = 14;
	static constexpr uint16_t H40LeftBorder = 13;
	static constexpr uint16_t RightBorder = 14;
	static constexpr uint16_t NtscTopBorder = 11;
	static constexpr uint16_t NtscBottomBorder = 8;
	static constexpr uint16_t PalV28TopBorder = 38;
	static constexpr uint16_t PalV28BottomBorder = 32;
	static constexpr uint16_t PalV30TopBorder = 30;
	static constexpr uint16_t PalV30BottomBorder = 24;

private:
	static constexpr uint32_t BgBufferWidth = 320 + MdVdp::H40LeftBorder + MdVdp::RightBorder;
	static constexpr uint8_t InitialFifoLatency = 3;

	enum class Event : uint8_t
	{
		VInterrupt,
		ReadHScroll,
		RenderLine,
		FetchSpriteAttributes,
		HBlankStart,
		HInterrupt,
		LatchRegisters,
		None
	};

	struct EventTime
	{
		uint16_t H;
		Event Type;
	};

	enum class DataLocation : uint8_t
	{
		Vram,
		Vram8Bit,
		Cram,
		Vsram,
		Invalid
	};

	enum class ForcedPlane : uint8_t
	{
		Backdrop = 0,
		Sprite = 1,
		PlaneA = 2,
		PlaneB = 3
	};

	enum class PixelMode : uint8_t
	{
		Normal = 0,
		Shadow = 1,
		Highlight = 2
	};

	//Decoded register values
	struct Regs
	{
		bool HIntEnabled;
		bool HvCounterStopped;
		bool LeftColumnBlank;

		bool DisplayEnabled;
		bool VIntEnabled;
		bool DmaEnabled;
		bool V30;
		bool Mode5;
		bool Vram128k;

		uint16_t PlaneATable;
		uint16_t WindowTable;
		uint16_t PlaneBTable;
		uint32_t SpriteTable;
		uint8_t BackdropColor;
		uint8_t HIntInterval;

		bool VScroll2Cell;
		uint8_t HScrollMode;

		bool H40;
		bool ShadowHighlight;
		uint8_t InterlaceMode; //0 = progressive, 1 = interlaced, 3 = interlaced double resolution

		uint16_t HScrollTable;
		uint8_t AutoIncrement;

		uint8_t HScrollSize; //Raw register bits (0 = 32, 1 = 64, 2 = invalid, 3 = 128)
		uint8_t VScrollSize;

		bool WindowRight;
		uint8_t WindowX; //In cells
		bool WindowDown;
		uint8_t WindowY; //In cells
	};

	struct FifoEntry
	{
		uint32_t Address;
		uint16_t Value;
		DataLocation Location;
		bool IsWrite;
		bool ByteWrite;
		bool SecondHalf;
		uint8_t Latency;
	};

	struct TilePixel
	{
		uint8_t Color;
		uint8_t Palette;
		bool Priority;
	};

	struct SpriteInfo
	{
		uint16_t Pattern;
		uint16_t Y;
		uint16_t X;
		uint8_t WidthCells;
		uint8_t HeightCells;
		uint8_t Palette;
		bool VFlip;
		bool HFlip;
		bool Priority;
	};

	struct SpriteBuffers
	{
		uint8_t ScannedIds[80];
		uint8_t ScannedCount;
		SpriteInfo Sprites[80];
		uint8_t SpriteCount;
		uint16_t LastTileAddresses[40];
		TilePixel Pixels[320];
	};

	struct CramDot
	{
		uint16_t X;
		uint16_t Color;
	};

	struct RasterLine
	{
		uint16_t Line;
		bool InVBorder;
	};

	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdCpu* _cpu = nullptr;
	MdZ80Cpu* _z80 = nullptr;
	MdMemoryManager* _memoryManager = nullptr;

	MdVdpState _state = {};
	uint64_t _lastMasterClock = 0;
	bool _isPal = false;

	uint8_t* _videoRam = nullptr;
	uint8_t _colorRam[MdConstants::ColorRamSize] = {}; //Big endian words (----bbb-ggg-rrr-)
	uint8_t _vsRam[MdConstants::VsRamSize] = {}; //Big endian words
	uint16_t _palette[0x40] = {}; //CRAM converted to the output format (BGR333)

	//Internal sprite attribute cache (4 bytes per sprite: Y MSB, Y LSB, size, link) - updated on VRAM writes, latched at HINT
	uint8_t _spriteCache[80 * 4] = {};
	uint8_t _latchedSpriteCache[80 * 4] = {};

	Regs _regs = {};
	Regs _latched = {};
	uint16_t _latchedHScroll[2] = {};
	uint16_t _latchedVScroll[2] = {};
	bool _debugDisplayDisabled = false;
	ForcedPlane _forcedPlane = ForcedPlane::Backdrop;

	//Line timing
	uint16_t _pixel = 0;
	uint8_t _eventIndex = 0;
	EventTime _events[8] = {};
	bool _inVblank = false;
	bool _vBorderForgotten = false;
	uint16_t _topBorder = MdVdp::NtscTopBorder;
	bool _frameH40 = false;
	bool _interlacedFrame = false;
	bool _interlacedOdd = false;
	uint8_t _lastScrollBPalettes[2] = {};

	//Control port / FIFO / DMA
	bool _dmaActive = false;
	bool _isWriteMode = false;
	uint8_t _locationBits = 0;
	DataLocation _location = DataLocation::Vram;
	uint32_t _controlAddress = 0;
	uint32_t _dataAddress = 0;
	bool _controlPending = false;

	FifoEntry _fifo[4] = {};
	uint8_t _fifoCount = 0;
	uint8_t _fifoPopIndex = 0;
	uint8_t _fifoPushIndex = 0;
	uint8_t _dmaLatency = 0;
	bool _fillDataReady = false;
	uint16_t _fillData = 0;
	bool _copyOddSlot = false;
	bool _displayEnablePending = false;

	uint32_t _dmaSource = 0;
	uint16_t _dmaLength = 0;

	bool _vIntPending = false;
	bool _hIntPending = false;
	uint16_t _hIntCounter = 0;
	bool _z80IntActive = false;

	uint16_t _hvLatch = 0;
	bool _hvLatched = false;

	//Sprites
	SpriteBuffers _sprites = {};
	SpriteBuffers _spritesOdd = {}; //Used for odd lines in interlace mode 2
	bool _spriteOverflow = false;
	bool _spriteCollision = false;
	bool _dotOverflowOnPrevLine = false;
	uint16_t _pixelsDisabledDuringHblank = 0;
	bool _spriteDisplayEnabled = false;
	uint16_t _displayEnabledPixel = 0;

	//Rendering
	TilePixel _planeAPixels[MdVdp::BgBufferWidth] = {};
	TilePixel _planeBPixels[MdVdp::BgBufferWidth] = {};
	CramDot _cramDots[32] = {};
	uint8_t _cramDotCount = 0;

	uint16_t* _outputBuffers[2] = {};
	uint16_t* _currentOutputBuffer = nullptr;
	uint32_t _frameWidth = 320;
	uint32_t _frameHeight = 224;

	bool _disableSprites = false;
	bool _disablePlaneA = false;
	bool _disablePlaneB = false;
	bool _disableWindow = false;
	bool _removeSpriteLimit = false;
	bool _showHorizontalBorder = false;
	bool _showVerticalBorder = false;

	static void DecodeRegs(uint8_t* raw, Regs& regs);
	void UpdateConfig();
	void RefreshPalette();

	//Timing
	void Exec(uint64_t runTo);
	void AdvanceToPixel(uint16_t endPixel);
	void AdvanceToNextLine();
	void ProcessSlot(uint16_t slot);
	void ProcessEvent(Event evt);
	void UpdateEventTimes();
	void HandleHResolutionChange();
	void EndFrame();

	uint16_t GetPixelsPerLine() { return _regs.H40 ? 420 : 342; }
	uint16_t GetActivePixels(const Regs& regs) { return regs.H40 ? 320 : 256; }
	uint16_t GetActiveScanlines(const Regs& regs) { return regs.V30 ? 240 : 224; }
	uint16_t GetActiveStartH() { return _regs.H40 ? 0x01A : 0x018; }
	uint16_t GetActiveEndH() { return _regs.H40 ? 0x15A : 0x118; }
	uint16_t GetHInterruptH() { return _regs.H40 ? 0x14A : 0x10A; }
	uint16_t GetLeftBorder(bool h40) { return h40 ? MdVdp::H40LeftBorder : MdVdp::H32LeftBorder; }
	uint16_t GetTopBorder(bool v30);
	uint16_t GetBottomBorder(bool v30);
	uint16_t GetRenderedLinesPerFrame();
	uint16_t GetScanlinesInFrame();

	static uint16_t MclkToPixel(uint32_t mclk, bool h40);
	static uint16_t PixelToInternalH(uint16_t pixel, bool h40);
	static uint16_t InternalHToPixel(uint16_t h, bool h40);

	uint16_t GetVCounter(uint32_t mclk, bool& vblankFlag);
	uint16_t GetHvCounterAt(uint32_t mclk);
	uint16_t GetStatus();

	//FIFO / DMA
	FifoEntry& GetFifoFront() { return _fifo[_fifoPopIndex]; }
	uint16_t GetNextSlotWord() { return _fifo[_fifoPushIndex].Value; }
	void PushFifo(FifoEntry entry);
	void PopFifo();
	FifoEntry CreateFifoEntry(uint16_t value);
	void HandleAccessSlot();
	void ProgressMemoryToVramDma(uint16_t slot, const bool* blankRefreshSlots);
	void ProgressVramFillDma();
	void ProgressVramCopyDma();
	void IncrementDmaSource();
	void DecrementDmaLength();
	void UpdateSpriteCache(uint32_t addr, uint8_t value);
	void WriteCram(uint8_t index, uint16_t value);
	void WriteVsram(uint8_t index, uint16_t value);
	static uint32_t Convert128kAddress(uint32_t addr);
	static DataLocation ParseLocation(uint8_t bits, bool isWrite);

	void WriteControlPort(uint16_t value);
	void WriteDataPort(uint16_t value);
	uint16_t ReadDataPort();
	void WriteRegister(uint8_t reg, uint8_t value);
	void RelatchRegister(uint8_t reg, bool changed);
	void RenderPartialLine(uint16_t fromPixel);
	void UpdateIrqState();
	void HandleDisplayEnableWrite(bool enabled);
	void IncrementPixelsDisabled(uint16_t pixel);

	//Rendering
	RasterLine GetRasterLine(uint16_t scanline);
	int32_t GetFrameBufferRow(RasterLine line);
	void RenderScanline(uint16_t scanline, uint16_t startPixel);
	void DoRenderScanline(uint16_t scanline, RasterLine line, uint16_t startPixel, int32_t row, bool oddLine);
	void RenderBackgrounds(uint16_t line, int16_t startCol, int16_t endCol, int16_t pixelOffset);
	void RenderWindow(uint16_t line, int16_t startCol, int16_t endCol, int16_t pixelOffset);
	void MergeLayers(RasterLine line, uint32_t row, uint32_t startCol, uint32_t endCol, int16_t pixelOffset, bool oddLine);
	void RenderVerticalBorderLine(uint16_t scanline, uint32_t row, uint16_t startPixel);
	void RenderLeftBorder(uint32_t row);
	void RenderRightBorder(uint32_t row);
	void RenderHorizontalBorderSpritePixel(uint32_t row, uint32_t col, uint16_t pixel, uint16_t baseSprite);
	void FillRow(uint32_t row, uint16_t startPixel, uint8_t colorIndex);
	void SetPixel(uint32_t row, uint32_t col, uint8_t colorIndex, PixelMode mode = PixelMode::Normal);
	uint16_t ReadVScroll(uint8_t plane, int16_t column);
	uint8_t GetBackdropIndex() { return _regs.BackdropColor; }

	void ScanSprites(uint16_t scanline);
	void DoSpriteScan(RasterLine line, SpriteBuffers& buffers);
	void FetchSpriteAttributes();
	void DoSpriteAttributeFetch(SpriteBuffers& buffers);
	void RenderSpritePixels(RasterLine line, SpriteBuffers& buffers);

	uint32_t GetCurrentScreenWidth();
	uint32_t GetCurrentScreenHeight();

public:
	~MdVdp();

	void Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80Cpu* z80, MdMemoryManager* memoryManager);
	void SetRegion(ConsoleRegion region);

	//Runs the VDP until it catches up to the specified master clock
	__forceinline void Run(uint64_t masterClock)
	{
		if(_lastMasterClock < masterClock) {
			Exec(masterClock);
		}
	}

	//Port access (port = address & 0x1F, word-aligned: $00 = data, $04 = control, $08 = HV counter)
	//Byte writes must be duplicated to both halves of the value by the caller, byte reads are extracted by the caller.
	uint16_t ReadPort(uint8_t port);
	uint16_t PeekPort(uint8_t port);
	void WritePort(uint8_t port, uint16_t value);

	//Undocumented debug register at $C0001C
	void WriteDebugRegister(uint16_t value);

	//Returns true when the 68000 must wait before performing the access (e.g full FIFO, read data not ready yet)
	bool IsCpuWriteBlocked(uint8_t port);
	bool IsCpuReadBlocked(uint8_t port);

	//Returns true while a 68000->VDP DMA holds the 68000's bus
	bool IsCpuBusLocked();

	//Called during the 68000's interrupt acknowledge cycle
	void AcknowledgeInterrupt(uint8_t level);

	//Called when the TH pin of a controller port transitions to high (when HV counter latching is enabled)
	void LatchHvCounter();

	uint32_t GetFrameCount() { return _state.FrameCount; }
	uint16_t GetScanline() { return _state.Scanline; }
	uint16_t GetHClock() { return _state.HClock; }
	uint16_t GetScanlineCount();
	uint16_t GetActiveLineCount() { return GetActiveScanlines(_regs); }

	//Position (in MCLK, relative to the start of the line) of the first active display pixel
	uint16_t GetActiveDisplayStartClock();

	//Size of the last completed frame
	uint32_t GetScreenWidth();
	uint32_t GetScreenHeight();

	//Size of the borders included in the output frame (0 when borders are hidden)
	uint16_t GetFrameLeftBorder() { return _showHorizontalBorder ? GetLeftBorder(_frameH40) : 0; }
	uint16_t GetFrameTopBorder() { return _showVerticalBorder ? _topBorder : 0; }

	MdVdpState& GetState();

	uint8_t* GetVideoRam() { return _videoRam; }
	uint8_t* GetColorRam() { return _colorRam; }
	uint8_t* GetVsRam() { return _vsRam; }
	uint8_t* GetSpriteCache() { return _spriteCache; }

	uint16_t* GetScreenBuffer(bool previousBuffer)
	{
		return previousBuffer ? ((_currentOutputBuffer == _outputBuffers[0]) ? _outputBuffers[1] : _outputBuffers[0]) : _currentOutputBuffer;
	}

	void DebugSendFrame();

	void Serialize(Serializer& s) override;
};
