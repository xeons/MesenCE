#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/SettingTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;
class MdCpu;
class MdZ80;
class MdMemoryManager;

class MdVdp final : public ISerializable
{
private:
	enum class SlotType : uint8_t
	{
		Other = 0,
		External = 1,
		Refresh = 2
	};

	enum class PixelMode : uint8_t
	{
		Normal = 0,
		Shadow = 1,
		Highlight = 2
	};

	struct LineTiming
	{
		uint16_t PixelCount;
		uint16_t SlotCount;
		uint16_t VCounterChangePixel; //Linear pixel index where the V counter increments (start of a VDP "line" here)
		uint16_t ActiveStart; //Linear pixel index of the first active display pixel
		uint16_t Width;
		uint16_t HBlankStart; //Internal H counter values
		uint16_t HBlankEnd;
		uint16_t PrepareSlot;
		uint16_t FinishSlot;
		uint16_t VerticalIrqSlot;
		uint16_t SlotClock[211];
		SlotType ActiveSlots[211];
		SlotType BlankSlots[211];
		uint16_t PixelAtClock[MdConstants::ClocksPerScanline];
	};

	static LineTiming _timings[2];
	static bool _timingsInitialized;

	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;
	MdCpu* _cpu = nullptr;
	MdZ80* _z80 = nullptr;
	MdMemoryManager* _memoryManager = nullptr;

	MdVdpState _state = {};
	uint64_t _lastMasterClock = 0;
	uint64_t _lineStartClock = 0;
	uint16_t _slotIndex = 0;
	ConsoleRegion _region = ConsoleRegion::Ntsc;

	uint8_t* _videoRam = nullptr;
	uint8_t _colorRam[MdConstants::ColorRamSize] = {}; //Stored as big endian words (----bbb-ggg-rrr-)
	uint8_t _vsRam[MdConstants::VsRamSize] = {}; //Stored as big endian words
	uint8_t _spriteCache[80 * 4] = {}; //Internal copy of the first 4 bytes of each sprite attribute table entry

	uint16_t* _outputBuffers[2] = {};
	uint16_t* _currentOutputBuffer = nullptr;
	uint16_t* _renderBuffer = nullptr;
	uint16_t _lineWidths[MdConstants::MaxScreenHeight] = {};
	uint32_t _frameWidth = 256;
	uint32_t _frameHeight = 224;

	//Per-frame/per-line latched timing values
	bool _lineH40 = false;
	bool _lineIsActive = false;
	bool _frameV30 = false;
	uint8_t _frameInterlaceMode = 0;
	uint16_t _totalLines = MdConstants::NtscScanlineCount;
	uint16_t _activeLines = 224;
	uint16_t _vCounterJumpFrom = 0xEA;
	uint16_t _vCounterJumpTo = 0x1E5;

	bool _z80IrqActive = false;
	uint64_t _z80IrqEndClock = 0;

	//Line rendering
	int16_t _renderLine = -1;
	uint16_t _renderRow = 0;
	uint16_t _renderWidth = 0;
	uint16_t _compositedX = 0;
	bool _prevLineDotOverflow = false;
	uint8_t _lineA[MdConstants::MaxScreenWidth] = {};
	uint8_t _lineB[MdConstants::MaxScreenWidth] = {};
	uint8_t _lineS[MdConstants::MaxScreenWidth] = {};
	uint16_t _palette[0x40] = {};

	bool _disableSprites = false;
	bool _disablePlaneA = false;
	bool _disablePlaneB = false;
	bool _disableWindow = false;
	bool _removeSpriteLimit = false;

	static void InitTimings();

	void Exec(uint64_t runTo);
	void ProcessSlot(uint16_t slot);
	void StartLine();
	void StartFrame();
	void EndFrame();
	void UpdateFrameTiming();

	void ProcessExternalSlot();
	bool DrainFifo();
	void ProcessDmaFetch();
	void ProcessDmaFillCopy();
	void ProcessRead();
	void UpdateDmaRegisters();
	void PushFifo(uint16_t value);

	void WriteVramByte(uint16_t addr, uint8_t value);
	void WriteCram(uint16_t addr, uint16_t value);
	void WriteVsram(uint16_t addr, uint16_t value);

	void WriteControlPort(uint16_t value);
	void WriteDataPort(uint16_t value);
	void WriteRegister(uint8_t reg, uint8_t value);
	void UpdateDecodedRegisters();
	void UpdateIrqState();
	void UpdateConfig();

	uint16_t GetStatus();
	uint16_t GetHCounter();
	uint16_t GetVCounter();
	uint16_t GetHvCounter();
	bool IsInterlaceMode2() { return _frameInterlaceMode == 3; }

	void PrepareLine();
	void FinishLine();
	void RenderPlane(uint8_t* out, bool planeA, uint16_t hscroll, uint16_t logicalLine, int16_t windowStart, int16_t windowEnd);
	void RenderWindow(uint8_t* out, uint16_t logicalLine, int16_t windowStart, int16_t windowEnd);
	void RenderSprites(uint16_t logicalLine);
	uint16_t GetCurrentRenderX();
	void CompositeTo(uint16_t x);
	void RefreshPalette();

	void BuildOutputFrame(uint16_t* out);

public:
	~MdVdp();

	void Init(Emulator* emu, MdConsole* console, MdCpu* cpu, MdZ80* z80, MdMemoryManager* memoryManager);
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

	//Size of the last completed frame
	uint32_t GetScreenWidth();
	uint32_t GetScreenHeight();

	MdVdpState& GetState() { return _state; }

	uint8_t* GetVideoRam() { return _videoRam; }
	uint8_t* GetColorRam() { return _colorRam; }
	uint8_t* GetVsRam() { return _vsRam; }

	uint16_t* GetScreenBuffer(bool previousBuffer)
	{
		return previousBuffer ? ((_currentOutputBuffer == _outputBuffers[0]) ? _outputBuffers[1] : _outputBuffers[0]) : _currentOutputBuffer;
	}

	void DebugSendFrame();

	void Serialize(Serializer& s) override;
};
