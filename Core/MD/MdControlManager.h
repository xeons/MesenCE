#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/SettingTypes.h"
#include "Shared/BaseControlManager.h"

class MdConsole;

class MdControlManager : public BaseControlManager
{
private:
	MdConsole* _console = nullptr;
	MdControlManagerState _state = {};
	MdConfig _prevConfig = {};

	uint8_t ReadDataPort(uint8_t port);
	void WriteDataPort(uint8_t port, uint8_t value);
	void WriteCtrlPort(uint8_t port, uint8_t value);
	void UpdateThState(uint8_t port);
	bool GetThOutput(uint8_t port);

public:
	MdControlManager(Emulator* emu, MdConsole* console);
	shared_ptr<BaseControlDevice> CreateControllerDevice(ControllerType type, uint8_t port) override;

	MdControlManagerState& GetState() { return _state; }

	void UpdateControlDevices() override;

	//I/O registers at $A10000-$A1001F (addr = register index, 0-15)
	uint8_t ReadRegister(uint8_t addr);
	uint8_t PeekRegister(uint8_t addr);
	void WriteRegister(uint8_t addr, uint8_t value);

	void Serialize(Serializer& s) override;
};
