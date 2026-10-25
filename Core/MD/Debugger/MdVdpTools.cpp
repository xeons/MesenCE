#include "pch.h"
#include "MD/Debugger/MdVdpTools.h"
#include "MD/MdTypes.h"
#include "MD/MdConsole.h"
#include "MD/MdVdp.h"
#include "Debugger/DebugTypes.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "Shared/SettingTypes.h"

MdVdpTools::MdVdpTools(Debugger* debugger, Emulator* emu, MdConsole* console) : PpuTools(debugger, emu)
{
	_console = console;
}

uint32_t MdVdpTools::GetRgbColor(uint16_t pixel)
{
	const uint8_t* levels;
	switch((pixel >> 9) & 0x03) {
		default:
		case 0: levels = _normalLevels; break;
		case 1: levels = _shadowLevels; break;
		case 2: levels = _highlightLevels; break;
	}

	uint8_t r = levels[pixel & 0x07];
	uint8_t g = levels[(pixel >> 3) & 0x07];
	uint8_t b = levels[(pixel >> 6) & 0x07];
	return 0xFF000000 | (r << 16) | (g << 8) | b;
}

void MdVdpTools::GetPlaneSize(MdVdpState& state, uint8_t layer, uint16_t& width, uint16_t& height)
{
	if(layer == 2) {
		//Window
		width = state.H40Mode ? 64 : 32;
		height = 32;
	} else {
		width = state.PlaneWidth;
		height = state.PlaneHeight;
		if(width * height > 64 * 64) {
			//Planes can't be larger than 64x64 cells (e.g 128x128 is not possible)
			height = (64 * 64) / width;
		}
	}
}

uint16_t MdVdpTools::GetPlaneAddress(MdVdpState& state, uint8_t layer)
{
	switch(layer) {
		default:
		case 0: return state.PlaneATableAddress;
		case 1: return state.PlaneBTableAddress;
		case 2: return state.WindowTableAddress;
	}
}

FrameInfo MdVdpTools::GetTilemapSize(GetTilemapOptions options, BaseState& baseState)
{
	MdVdpState& state = (MdVdpState&)baseState;
	uint16_t width, height;
	GetPlaneSize(state, options.Layer, width, height);
	uint32_t tileHeight = state.InterlaceMode == 3 ? 16 : 8;
	return { (uint32_t)width * 8, (uint32_t)height * tileHeight };
}

DebugTilemapInfo MdVdpTools::GetTilemap(GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint32_t* palette, uint32_t* outBuffer)
{
	MdVdpState& state = (MdVdpState&)baseState;

	bool im2 = state.InterlaceMode == 3;
	uint8_t tileHeight = im2 ? 16 : 8;

	uint16_t width, height;
	GetPlaneSize(state, options.Layer, width, height);
	uint16_t baseAddr = GetPlaneAddress(state, options.Layer);

	uint32_t rowSize = width * 8;
	uint32_t bgColor = GetTilemapBackgroundColor(options.Background, palette[state.BackgroundColorIndex & 0x3F]);

	uint8_t colorMask = 0x3F;
	if(options.DisplayMode == TilemapDisplayMode::Grayscale) {
		palette = (uint32_t*)_grayscaleColorsBpp4;
		colorMask = 0x0F;
	}

	for(uint16_t row = 0; row < height; row++) {
		for(uint16_t column = 0; column < width; column++) {
			uint16_t entryAddr = (baseAddr + (row * width + column) * 2) & 0xFFFE;
			uint16_t entry = (vram[entryAddr] << 8) | vram[entryAddr + 1];

			uint16_t tileIndex = entry & 0x7FF;
			uint8_t paletteOffset = ((entry >> 13) & 0x03) * 16;
			bool vMirror = entry & 0x1000;
			bool hMirror = entry & 0x0800;
			uint16_t tileAddr = im2 ? ((tileIndex & 0x3FF) << 6) : (tileIndex << 5);

			for(int y = 0; y < tileHeight; y++) {
				uint8_t tileRow = vMirror ? (tileHeight - 1 - y) : y;
				uint16_t rowAddr = (tileAddr + tileRow * 4) & 0xFFFF;
				for(int x = 0; x < 8; x++) {
					uint8_t tileColumn = hMirror ? 7 - x : x;
					uint8_t color = GetTilePixelColor<TileFormat::WsBpp4Packed>(vram, 0xFFFF, rowAddr, tileColumn);
					uint32_t outPos = (row * tileHeight + y) * rowSize + column * 8 + x;
					outBuffer[outPos] = color == 0 ? bgColor : palette[(paletteOffset + color) & colorMask];
				}
			}
		}
	}

	DebugTilemapInfo result = {};
	result.Bpp = 4;
	result.Format = TileFormat::WsBpp4Packed;
	result.TileWidth = 8;
	result.TileHeight = tileHeight;
	result.ColumnCount = width;
	result.RowCount = height;
	result.TilemapAddress = baseAddr;
	result.TilesetAddress = 0;

	result.ScrollWidth = state.H40Mode ? 320 : 256;
	result.ScrollHeight = (state.V30Mode ? 240 : 224) * (im2 ? 2 : 1);

	if(options.Layer < 2) {
		//Show the scroll values used for the top of the screen (line/column scroll modes can't be represented)
		uint16_t hscrollAddr = (state.HScrollTableAddress + options.Layer * 2) & 0xFFFE;
		uint16_t hscroll = ((vram[hscrollAddr] << 8) | vram[hscrollAddr + 1]) & 0x3FF;
		uint8_t* vsram = _console->GetVdp()->GetVsRam();
		uint16_t vscroll = ((vsram[options.Layer * 2] << 8) | vsram[options.Layer * 2 + 1]) & (im2 ? 0x7FF : 0x3FF);

		uint32_t planeWidth = width * 8;
		uint32_t planeHeight = height * tileHeight;
		result.ScrollX = (uint32_t)(planeWidth - (hscroll % planeWidth)) % planeWidth;
		result.ScrollY = vscroll % planeHeight;
	} else {
		result.ScrollX = 0;
		result.ScrollY = 0;
	}

	return result;
}

DebugTilemapTileInfo MdVdpTools::GetTilemapTileInfo(uint32_t x, uint32_t y, uint8_t* vram, GetTilemapOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	DebugTilemapTileInfo result = {};

	FrameInfo size = GetTilemapSize(options, baseState);
	if(x >= size.Width || y >= size.Height) {
		return result;
	}

	MdVdpState& state = (MdVdpState&)baseState;
	bool im2 = state.InterlaceMode == 3;
	uint8_t tileHeight = im2 ? 16 : 8;

	uint16_t width, height;
	GetPlaneSize(state, options.Layer, width, height);
	uint16_t baseAddr = GetPlaneAddress(state, options.Layer);

	uint16_t row = y / tileHeight;
	uint16_t column = x / 8;
	uint16_t entryAddr = (baseAddr + (row * width + column) * 2) & 0xFFFE;
	uint16_t entry = (vram[entryAddr] << 8) | vram[entryAddr + 1];
	uint16_t tileIndex = entry & 0x7FF;

	result.Row = row;
	result.Column = column;
	result.Width = 8;
	result.Height = tileHeight;
	result.TileMapAddress = entryAddr;
	result.TileIndex = tileIndex;
	result.AddAddress(im2 ? ((tileIndex & 0x3FF) << 6) : (tileIndex << 5));
	result.PaletteIndex = (entry >> 13) & 0x03;
	result.PaletteAddress = result.PaletteIndex * 16;
	result.HighPriority = (entry & 0x8000) ? NullableBoolean::True : NullableBoolean::False;
	result.VerticalMirroring = (entry & 0x1000) ? NullableBoolean::True : NullableBoolean::False;
	result.HorizontalMirroring = (entry & 0x0800) ? NullableBoolean::True : NullableBoolean::False;
	return result;
}

DebugSpritePreviewInfo MdVdpTools::GetSpritePreviewInfo(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState)
{
	MdVdpState& state = (MdVdpState&)baseState;
	bool im2 = state.InterlaceMode == 3;

	DebugSpritePreviewInfo info = {};
	info.Width = 512;
	info.Height = im2 ? 1024 : 512;
	info.SpriteCount = state.H40Mode ? 80 : 64;

	//Sprite coordinates are offset by 128 (256 vertically in interlace mode 2)
	info.CoordOffsetX = 128;
	info.CoordOffsetY = im2 ? 256 : 128;

	info.VisibleX = 128;
	info.VisibleY = im2 ? 256 : 128;
	info.VisibleWidth = state.H40Mode ? 320 : 256;
	info.VisibleHeight = (state.V30Mode ? 240 : 224) * (im2 ? 2 : 1);

	info.WrapBottomToTop = false;
	info.WrapRightToLeft = false;
	return info;
}

void MdVdpTools::GetSpriteInfo(DebugSpriteInfo& sprite, uint32_t* spritePreview, uint16_t i, MdVdpState& state, uint8_t* vram, uint8_t* spriteCache, uint32_t* palette)
{
	bool im2 = state.InterlaceMode == 3;
	uint8_t tileHeight = im2 ? 16 : 8;
	uint16_t yMask = im2 ? 0x3FF : 0x1FF;
	int yOffset = im2 ? 256 : 128;

	//The VDP uses its internal copy of the first 4 bytes of each entry (Y, size and link)
	uint8_t* cache = spriteCache + i * 4;
	uint16_t entryAddr = (state.SpriteTableAddress + i * 8) & 0xFFFF;
	uint16_t pattern = (vram[(entryAddr + 4) & 0xFFFF] << 8) | vram[(entryAddr + 5) & 0xFFFF];
	uint16_t xpos = ((vram[(entryAddr + 6) & 0xFFFF] << 8) | vram[(entryAddr + 7) & 0xFFFF]) & 0x1FF;
	uint16_t ypos = ((cache[0] << 8) | cache[1]) & yMask;

	uint8_t heightCells = (cache[2] & 0x03) + 1;
	uint8_t widthCells = ((cache[2] >> 2) & 0x03) + 1;
	bool hMirror = pattern & 0x0800;
	bool vMirror = pattern & 0x1000;

	sprite.SpriteIndex = i;
	sprite.Bpp = 4;
	sprite.Format = TileFormat::WsBpp4Packed;
	sprite.UseExtendedVram = false;
	sprite.RawX = xpos;
	sprite.RawY = ypos;
	sprite.X = xpos - 128;
	sprite.Y = ypos - yOffset;
	sprite.Width = widthCells * 8;
	sprite.Height = heightCells * tileHeight;
	sprite.TileIndex = pattern & 0x7FF;
	sprite.Palette = (pattern >> 13) & 0x03;
	sprite.PaletteAddress = sprite.Palette * 16;
	sprite.Priority = (pattern & 0x8000) ? DebugSpritePriority::Foreground : DebugSpritePriority::Background;
	sprite.HorizontalMirror = hMirror ? NullableBoolean::True : NullableBoolean::False;
	sprite.VerticalMirror = vMirror ? NullableBoolean::True : NullableBoolean::False;

	int screenWidth = state.H40Mode ? 320 : 256;
	int screenHeight = (state.V30Mode ? 240 : 224) * (im2 ? 2 : 1);
	bool onScreen = sprite.X + sprite.Width > 0 && sprite.X < screenWidth && sprite.Y + sprite.Height > 0 && sprite.Y < screenHeight;
	sprite.Visibility = onScreen ? SpriteVisibility::Visible : SpriteVisibility::Offscreen;

	auto getTileAddr = [&](uint16_t tile) -> uint16_t {
		tile &= 0x7FF;
		return im2 ? ((tile & 0x3FF) << 6) : (tile << 5);
	};

	//Tiles are arranged in column-major order
	sprite.TileCount = 0;
	for(int row = 0; row < heightCells; row++) {
		for(int column = 0; column < widthCells; column++) {
			sprite.TileAddresses[sprite.TileCount++] = getTileAddr(sprite.TileIndex + column * heightCells + row);
		}
	}
	sprite.TileAddress = getTileAddr(sprite.TileIndex);

	for(int y = 0; y < sprite.Height; y++) {
		int srcY = vMirror ? (sprite.Height - 1 - y) : y;
		int cellRow = srcY / tileHeight;
		int pixelRow = srcY % tileHeight;
		for(int x = 0; x < sprite.Width; x++) {
			int srcX = hMirror ? (sprite.Width - 1 - x) : x;
			int cellColumn = srcX / 8;
			uint16_t rowAddr = (getTileAddr(sprite.TileIndex + cellColumn * heightCells + cellRow) + pixelRow * 4) & 0xFFFF;
			uint8_t color = GetTilePixelColor<TileFormat::WsBpp4Packed>(vram, 0xFFFF, rowAddr, srcX & 0x07);

			uint32_t outOffset = y * sprite.Width + x;
			spritePreview[outOffset] = color > 0 ? palette[sprite.PaletteAddress + color] : 0;
		}
	}
}

void MdVdpTools::GetSpritePreview(GetSpritePreviewOptions options, MdVdpState& state, DebugSpriteInfo* sprites, uint8_t* linkOrder, uint8_t linkCount, uint32_t* spritePreviews, uint32_t* palette, uint32_t* outBuffer)
{
	DebugSpritePreviewInfo info = GetSpritePreviewInfo(options, state, state);

	uint32_t bgColor = GetSpriteBackgroundColor(options.Background, palette, false);
	uint32_t darkBgColor = GetSpriteBackgroundColor(options.Background, palette, true);

	std::fill(outBuffer, outBuffer + info.Width * info.Height, darkBgColor);
	for(uint32_t y = info.VisibleY; y < info.VisibleY + info.VisibleHeight; y++) {
		std::fill(outBuffer + y * info.Width + info.VisibleX, outBuffer + y * info.Width + info.VisibleX + info.VisibleWidth, bgColor);
	}

	//Sprites earlier in the link list have priority, draw them last
	for(int i = linkCount - 1; i >= 0; i--) {
		DebugSpriteInfo& sprite = sprites[linkOrder[i]];
		uint32_t* spritePreview = spritePreviews + linkOrder[i] * _spritePreviewSize;

		for(int y = 0; y < sprite.Height; y++) {
			int posY = sprite.RawY + y;
			if(posY >= (int)info.Height) {
				break;
			}

			for(int x = 0; x < sprite.Width; x++) {
				int posX = sprite.RawX + x;
				if(posX >= (int)info.Width) {
					break;
				}

				uint32_t color = spritePreview[y * sprite.Width + x];
				if(color != 0) {
					outBuffer[posY * info.Width + posX] = color;
				}
			}
		}
	}

	for(uint8_t i = 0; i < info.SpriteCount; i++) {
		//Make transparent pixels use the background color in the individual sprite previews
		DebugSpriteInfo& sprite = sprites[i];
		uint32_t* spritePreview = spritePreviews + i * _spritePreviewSize;
		for(int j = 0, len = sprite.Width * sprite.Height; j < len; j++) {
			if(spritePreview[j] == 0) {
				spritePreview[j] = bgColor;
			}
		}
	}
}

void MdVdpTools::GetSpriteList(GetSpritePreviewOptions options, BaseState& baseState, BaseState& ppuToolsState, uint8_t* vram, uint8_t* oamRam, uint32_t* palette, DebugSpriteInfo outBuffer[], uint32_t* spritePreviews, uint32_t* screenPreview)
{
	MdVdpState& state = (MdVdpState&)baseState;
	uint8_t* spriteCache = _console->GetVdp()->GetSpriteCache();
	uint8_t spriteCount = state.H40Mode ? 80 : 64;

	for(int i = 0; i < spriteCount; i++) {
		outBuffer[i].Init();
		GetSpriteInfo(outBuffer[i], spritePreviews + (i * _spritePreviewSize), i, state, vram, spriteCache, palette);
	}

	//Follow the link list, sprites that aren't part of it are never displayed
	bool inList[80] = {};
	uint8_t linkOrder[80];
	uint8_t linkCount = 0;
	uint8_t index = 0;
	while(linkCount < spriteCount && !inList[index]) {
		inList[index] = true;
		linkOrder[linkCount++] = index;
		index = spriteCache[index * 4 + 3] & 0x7F;
		if(index == 0 || index >= spriteCount) {
			break;
		}
	}

	for(int i = 0; i < spriteCount; i++) {
		if(!inList[i]) {
			outBuffer[i].Visibility = SpriteVisibility::Disabled;
		}
	}

	GetSpritePreview(options, state, outBuffer, linkOrder, linkCount, spritePreviews, palette, screenPreview);
}

DebugPaletteInfo MdVdpTools::GetPaletteInfo(GetPaletteInfoOptions options)
{
	DebugPaletteInfo info = {};
	info.ColorCount = 64;
	info.BgColorCount = 64;
	info.SpriteColorCount = 64;
	info.SpritePaletteOffset = 0;
	info.ColorsPerPalette = 16;

	info.HasMemType = true;
	info.PaletteMemType = MemoryType::MdColorRam;
	info.PaletteMemOffset = 0;
	info.RawFormat = RawPaletteFormat::Rgb333;

	uint8_t* cram = _debugger->GetMemoryDumper()->GetMemoryBuffer(MemoryType::MdColorRam);
	for(int i = 0; i < 64; i++) {
		uint16_t value = (cram[i * 2] << 8) | cram[i * 2 + 1];
		info.RawPalette[i] = ((value >> 3) & 0x1C0) | ((value >> 2) & 0x38) | ((value >> 1) & 0x07);
		info.RgbPalette[i] = GetRgbColor((uint16_t)info.RawPalette[i]);
	}

	return info;
}

void MdVdpTools::SetPaletteColor(int32_t colorIndex, uint32_t color)
{
	if(colorIndex < 0 || colorIndex >= 64) {
		return;
	}

	uint8_t r, g, b;
	if(color <= 0x1FF) {
		//Raw 9-bit color (BBBGGGRRR)
		r = color & 0x07;
		g = (color >> 3) & 0x07;
		b = (color >> 6) & 0x07;
	} else {
		//ARGB color, pick the closest DAC level for each component
		auto getClosest = [](uint8_t value) -> uint8_t {
			uint8_t best = 0;
			for(uint8_t i = 1; i < 8; i++) {
				if(std::abs((int)_normalLevels[i] - value) < std::abs((int)_normalLevels[best] - value)) {
					best = i;
				}
			}
			return best;
		};
		r = getClosest((color >> 16) & 0xFF);
		g = getClosest((color >> 8) & 0xFF);
		b = getClosest(color & 0xFF);
	}

	//CRAM entries are stored as big endian words: ----bbb-ggg-rrr-
	MemoryDumper* dumper = _debugger->GetMemoryDumper();
	dumper->SetMemoryValue(MemoryType::MdColorRam, colorIndex * 2, b << 1);
	dumper->SetMemoryValue(MemoryType::MdColorRam, colorIndex * 2 + 1, (g << 5) | (r << 1));
}
