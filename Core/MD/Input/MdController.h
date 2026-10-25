#pragma once
#include "pch.h"
#include "Shared/BaseControlDevice.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/InputHud.h"
#include "Utilities/Serializer.h"

//3-button control pad and 6-button fighting pad
class MdController : public BaseControlDevice
{
private:
	//The 6-button pad's TH counter resets when TH stops toggling for ~1.5ms
	static constexpr uint32_t SixButtonTimeoutMs = 1500;

	uint32_t _turboSpeed = 0;
	bool _sixButton = false;

	bool _thHigh = true;
	uint8_t _thCounter = 0;
	uint64_t _lastThChange = 0;

	void UpdateTimeout()
	{
		if(_sixButton && _thCounter != 0) {
			uint64_t elapsed = _emu->GetMasterClock() - _lastThChange;
			if(elapsed * 1000000 >= (uint64_t)_emu->GetMasterClockRate() * SixButtonTimeoutMs) {
				_thCounter = 0;
			}
		}
	}

protected:
	string GetKeyNames() override
	{
		return _sixButton ? "UDLRABCSXYZM" : "UDLRABCS";
	}

	void InternalSetStateFromInput() override
	{
		bool turboOn = IsTurboOn(_turboSpeed);

		for(KeyMapping& keyMapping : _keyMappings) {
			SetPressedState(Buttons::Up, keyMapping.Up);
			SetPressedState(Buttons::Down, keyMapping.Down);
			SetPressedState(Buttons::Left, keyMapping.Left);
			SetPressedState(Buttons::Right, keyMapping.Right);
			SetPressedState(Buttons::A, keyMapping.A);
			SetPressedState(Buttons::B, keyMapping.B);
			SetPressedState(Buttons::C, keyMapping.X);
			SetPressedState(Buttons::Start, keyMapping.Start);

			if(_sixButton) {
				SetPressedState(Buttons::X, keyMapping.Y);
				SetPressedState(Buttons::Y, keyMapping.L);
				SetPressedState(Buttons::Z, keyMapping.R);
				SetPressedState(Buttons::Mode, keyMapping.Select);
			}

			if(turboOn) {
				SetPressedState(Buttons::A, keyMapping.TurboA);
				SetPressedState(Buttons::B, keyMapping.TurboB);
				SetPressedState(Buttons::C, keyMapping.TurboX);
			}
		}

		if(!_emu->GetSettings()->GetMdConfig().AllowInvalidInput) {
			//If both U+D or L+R are pressed at the same time, act as if neither is pressed
			if(IsPressed(Buttons::Up) && IsPressed(Buttons::Down)) {
				ClearBit(Buttons::Down);
				ClearBit(Buttons::Up);
			}
			if(IsPressed(Buttons::Left) && IsPressed(Buttons::Right)) {
				ClearBit(Buttons::Left);
				ClearBit(Buttons::Right);
			}
		}
	}

	void RefreshStateBuffer() override
	{
	}

	void Serialize(Serializer& s) override
	{
		BaseControlDevice::Serialize(s);
		SV(_thHigh);
		SV(_thCounter);
		SV(_lastThChange);
	}

public:
	enum Buttons
	{
		Up = 0,
		Down,
		Left,
		Right,
		A,
		B,
		C,
		Start,
		X,
		Y,
		Z,
		Mode
	};

	MdController(Emulator* emu, ControllerType type, uint8_t port, KeyMappingSet keyMappings) : BaseControlDevice(emu, type, port, keyMappings)
	{
		_turboSpeed = keyMappings.TurboSpeed;
		_sixButton = type == ControllerType::MdController6Button;
	}

	//Returns the state of pins 0-5 (active low) based on the current TH state
	uint8_t ReadRam(uint16_t addr) override
	{
		UpdateTimeout();

		uint8_t value = 0x3F;
		auto clear = [&](uint8_t bit, bool pressed) {
			if(pressed) {
				value &= ~bit;
			}
		};

		if(_thHigh) {
			if(_sixButton && _thCounter == 3) {
				//4th TH high: ?1CBMXYZ
				clear(0x01, IsPressed(Buttons::Z));
				clear(0x02, IsPressed(Buttons::Y));
				clear(0x04, IsPressed(Buttons::X));
				clear(0x08, IsPressed(Buttons::Mode));
			} else {
				//?1CBRLDU
				clear(0x01, IsPressed(Buttons::Up));
				clear(0x02, IsPressed(Buttons::Down));
				clear(0x04, IsPressed(Buttons::Left));
				clear(0x08, IsPressed(Buttons::Right));
			}
			clear(0x10, IsPressed(Buttons::B));
			clear(0x20, IsPressed(Buttons::C));
		} else {
			if(_sixButton && _thCounter == 2) {
				//3rd TH low: ?0SA0000 (identifies a 6-button pad)
				value &= ~0x0F;
			} else if(_sixButton && _thCounter == 3) {
				//4th TH low: ?0SA1111
			} else {
				//?0SA00DU
				value &= ~0x0C;
				clear(0x01, IsPressed(Buttons::Up));
				clear(0x02, IsPressed(Buttons::Down));
			}
			clear(0x10, IsPressed(Buttons::A));
			clear(0x20, IsPressed(Buttons::Start));
		}

		return value;
	}

	//Called when the TH line's state changes (value = TH level)
	void WriteRam(uint16_t addr, uint8_t value) override
	{
		bool thHigh = value != 0;
		if(thHigh == _thHigh) {
			return;
		}

		UpdateTimeout();
		if(thHigh && _sixButton) {
			_thCounter = (_thCounter + 1) & 0x03;
		}
		_lastThChange = _emu->GetMasterClock();
		_thHigh = thHigh;
	}

	void InternalDrawController(InputHud& hud) override
	{
		hud.DrawOutline(35, 14);

		hud.DrawButton(5, 3, 3, 3, IsPressed(Buttons::Up));
		hud.DrawButton(5, 9, 3, 3, IsPressed(Buttons::Down));
		hud.DrawButton(2, 6, 3, 3, IsPressed(Buttons::Left));
		hud.DrawButton(8, 6, 3, 3, IsPressed(Buttons::Right));
		hud.DrawButton(5, 6, 3, 3, false);

		hud.DrawButton(21, 8, 3, 3, IsPressed(Buttons::A));
		hud.DrawButton(25, 8, 3, 3, IsPressed(Buttons::B));
		hud.DrawButton(29, 8, 3, 3, IsPressed(Buttons::C));

		if(_sixButton) {
			hud.DrawButton(21, 3, 3, 3, IsPressed(Buttons::X));
			hud.DrawButton(25, 3, 3, 3, IsPressed(Buttons::Y));
			hud.DrawButton(29, 3, 3, 3, IsPressed(Buttons::Z));
			hud.DrawButton(13, 4, 4, 2, IsPressed(Buttons::Mode));
		}

		hud.DrawButton(13, 9, 4, 2, IsPressed(Buttons::Start));

		hud.DrawNumber(_port + 1, 16, 1);
	}

	vector<DeviceButtonName> GetKeyNameAssociations() override
	{
		return {
			{ "a", Buttons::A },
			{ "b", Buttons::B },
			{ "c", Buttons::C },
			{ "x", Buttons::X },
			{ "y", Buttons::Y },
			{ "z", Buttons::Z },
			{ "start", Buttons::Start },
			{ "mode", Buttons::Mode },
			{ "up", Buttons::Up },
			{ "down", Buttons::Down },
			{ "left", Buttons::Left },
			{ "right", Buttons::Right },
		};
	}
};
