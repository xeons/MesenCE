#pragma once
#include "pch.h"
#include "Debugger/PpuTools.h"

class Debugger;
class Emulator;
class MdConsole;
struct MdVdpState;

class MdVdpTools final : public PpuTools
{
private:
	//Measured output levels of the VDP's DAC for each 3-bit component (normal, shadow and highlight)
	static constexpr uint8_t _normalLevels[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
	static constexpr uint8_t _shadowLevels[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
	static constexpr uint8_t _highlightLevels[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

	MdConsole* _console = nullptr;

	void GetPlaneSize(MdVdpState& state, uint8_t layer, uint16_t& width, uint16_t& height);
	uint16_t GetPlaneAddress(MdVdpState& state, uint8_t layer);

	void GetSpriteInfo(DebugSpriteInfo& sprite, uint32_t* spritePreview, uint16_t spriteIndex, MdVdpState& state, uint8_t* vram, uint8_t* spriteCache, uint32_t* palette);
	void GetSpritePreview(GetSpritePreviewOptions options, MdVdpState& state, DebugSpriteInfo* sprites, uint8_t* linkOrder, uint8_t linkCount, uint32_t* spritePreviews, uint32_t* palette, uint32_t* outBuffer);

public:
	MdVdpTools(Debugger* debugger, Emulator* emu, MdConsole* console);

	//Converts a VDP output pixel (9-bit BGR color + shadow/highlight mode in bits 9-10) to ARGB
	static uint32_t GetRgbColor(uint16_t pixel);

	DebugTilemapInfo GetTilemap(GetTilemapOptions options, BaseState& state, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer) override;
	FrameInfo GetTilemapSize(GetTilemapOptions options, BaseState& state) override;
	DebugTilemapTileInfo GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState) override;

	DebugSpritePreviewInfo GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& state, BaseState& ppuToolsState) override;
	void GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview) override;

	DebugPaletteInfo GetPaletteInfo(GetPaletteInfoOptions options) override;
	void SetPaletteColor(int32_t colorIndex, uint32_t color) override;
};
