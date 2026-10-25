#pragma once
#include "pch.h"
#include "MD/MdConsole.h"
#include "MD/MdTypes.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/EmuSettings.h"
#include "Shared/Emulator.h"

class MdDefaultVideoFilter : public BaseVideoFilter
{
private:
	//Measured output levels of the VDP's DAC for each 3-bit component (normal, shadow and highlight)
	static constexpr uint8_t _normalLevels[8] = { 0, 52, 87, 116, 144, 172, 206, 255 };
	static constexpr uint8_t _shadowLevels[8] = { 0, 29, 52, 70, 87, 101, 116, 130 };
	static constexpr uint8_t _highlightLevels[8] = { 130, 144, 158, 172, 187, 206, 228, 255 };

	uint32_t _calculatedPalette[0x800] = {};
	VideoConfig _videoConfig = {};
	MdConsole* _console = nullptr;

protected:
	void OnBeforeApplyFilter() override
	{
		VideoConfig& config = _emu->GetSettings()->GetVideoConfig();
		if(_videoConfig.Hue != config.Hue || _videoConfig.Saturation != config.Saturation || _videoConfig.Contrast != config.Contrast || _videoConfig.Brightness != config.Brightness) {
			InitLookupTable();
		}
		_videoConfig = config;
	}

	void InitLookupTable()
	{
		VideoConfig config = _emu->GetSettings()->GetVideoConfig();

		InitConversionMatrix(config.Hue, config.Saturation);

		for(int i = 0; i < 0x800; i++) {
			const uint8_t* levels;
			switch((i >> 9) & 0x03) {
				default:
				case 0: levels = _normalLevels; break;
				case 1: levels = _shadowLevels; break;
				case 2: levels = _highlightLevels; break;
			}

			uint8_t r = levels[i & 0x07];
			uint8_t g = levels[(i >> 3) & 0x07];
			uint8_t b = levels[(i >> 6) & 0x07];

			if(config.Hue != 0 || config.Saturation != 0 || config.Brightness != 0 || config.Contrast != 0) {
				ApplyColorOptions(r, g, b, config.Brightness, config.Contrast);
			}
			_calculatedPalette[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
		}

		_videoConfig = config;
	}

public:
	MdDefaultVideoFilter(Emulator* emu, MdConsole* console) : BaseVideoFilter(emu)
	{
		_console = console;
		InitLookupTable();
	}

	OverscanDimensions GetOverscan() override
	{
		OverscanDimensions overscan = BaseVideoFilter::GetOverscan();
		if(_baseFrameInfo.Height > 300) {
			//Interlace mode 2 outputs twice as many lines
			overscan.Top *= 2;
			overscan.Bottom *= 2;
		}
		return overscan;
	}

	void ApplyFilter(uint16_t* ppuOutputBuffer) override
	{
		uint32_t* out = GetOutputBuffer();
		FrameInfo frame = _frameInfo;
		OverscanDimensions overscan = GetOverscan();

		for(uint32_t y = 0; y < frame.Height; y++) {
			uint16_t* in = ppuOutputBuffer + (y + overscan.Top) * _baseFrameInfo.Width + overscan.Left;
			for(uint32_t x = 0; x < frame.Width; x++) {
				out[y * frame.Width + x] = _calculatedPalette[in[x] & 0x7FF];
			}
		}
	}
};
