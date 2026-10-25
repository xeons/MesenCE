#include "pch.h"
#include "MD/MdControlManager.h"
#include "MD/MdConsole.h"
#include "MD/Input/MdController.h"
#include "Shared/Emulator.h"
#include "Shared/EmuSettings.h"
#include "Shared/KeyManager.h"
#include "Utilities/Serializer.h"

MdControlManager::MdControlManager(Emulator* emu, MdConsole* console) : BaseControlManager(emu, CpuType::Md)
{
	_emu = emu;
	_console = console;

	for(int i = 0; i < 3; i++) {
		_state.Ports[i].Data = 0x7F;
		_state.Ports[i].Ctrl = 0x00;
		_state.Ports[i].TxData = 0xFF;
		_state.Ports[i].SerialCtrl = 0x00;
	}
}

shared_ptr<BaseControlDevice> MdControlManager::CreateControllerDevice(ControllerType type, uint8_t port)
{
	shared_ptr<BaseControlDevice> device;

	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();

	KeyMappingSet keys;
	switch(port) {
		default:
		case 0: keys = cfg.Port1.Keys; break;
		case 1: keys = cfg.Port2.Keys; break;
	}

	switch(type) {
		default:
		case ControllerType::None: break;

		case ControllerType::MdController3Button:
		case ControllerType::MdController6Button:
			device.reset(new MdController(_emu, type, port, keys));
			break;
	}

	return device;
}

void MdControlManager::UpdateControlDevices()
{
	MdConfig& cfg = _emu->GetSettings()->GetMdConfig();
	if(_emu->GetSettings()->IsEqual(_prevConfig, cfg) && _controlDevices.size() > 0) {
		//Do nothing if configuration is unchanged
		return;
	}

	auto lock = _deviceLock.AcquireSafe();

	ClearDevices();

	for(int i = 0; i < 2; i++) {
		shared_ptr<BaseControlDevice> device = CreateControllerDevice(i == 0 ? cfg.Port1.Type : cfg.Port2.Type, i);
		if(device) {
			RegisterControlDevice(device);
		}
	}

	for(int i = 0; i < 2; i++) {
		UpdateThState(i);
	}
}

bool MdControlManager::GetThOutput(uint8_t port)
{
	//TH is pulled up when it is configured as an input
	MdIoPortState& p = _state.Ports[port];
	return (p.Ctrl & 0x40) ? (p.Data & 0x40) != 0 : true;
}

void MdControlManager::UpdateThState(uint8_t port)
{
	bool th = GetThOutput(port);
	for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
		if(device->IsConnected() && device->GetPort() == port) {
			device->WriteRam(0, th ? 1 : 0);
		}
	}
}

uint8_t MdControlManager::ReadDataPort(uint8_t port)
{
	uint8_t input = 0x7F;
	if(port < 2) {
		for(shared_ptr<BaseControlDevice>& device : _controlDevices) {
			if(device->IsConnected() && device->GetPort() == port) {
				input &= device->ReadRam(0) | 0x40;
			}
		}
	}

	//Pins configured as outputs return the data register's value, bit 7 is a plain latch
	MdIoPortState& p = _state.Ports[port];
	uint8_t outputMask = p.Ctrl | 0x80;
	return (p.Data & outputMask) | (input & ~outputMask);
}

void MdControlManager::WriteDataPort(uint8_t port, uint8_t value)
{
	_state.Ports[port].Data = value;
	if(port < 2) {
		UpdateThState(port);
	}
}

void MdControlManager::WriteCtrlPort(uint8_t port, uint8_t value)
{
	_state.Ports[port].Ctrl = value;
	if(port < 2) {
		UpdateThState(port);
	}
}

uint8_t MdControlManager::ReadRegister(uint8_t addr)
{
	if(addr >= 1 && addr <= 3) {
		SetInputReadFlag();
	}
	return PeekRegister(addr);
}

uint8_t MdControlManager::PeekRegister(uint8_t addr)
{
	switch(addr & 0x0F) {
		case 0x00: {
			//Version register: overseas, PAL, no expansion unit attached, version 0 (no TMSS)
			uint8_t value = 0x20;
			value |= _console->IsOverseas() ? 0x80 : 0;
			value |= _console->IsPal() ? 0x40 : 0;
			return value;
		}

		case 0x01: case 0x02: case 0x03: return ReadDataPort((addr & 0x0F) - 1);
		case 0x04: case 0x05: case 0x06: return _state.Ports[(addr & 0x0F) - 4].Ctrl;

		case 0x07: return _state.Ports[0].TxData;
		case 0x08: return 0;
		case 0x09: return _state.Ports[0].SerialCtrl;
		case 0x0A: return _state.Ports[1].TxData;
		case 0x0B: return 0;
		case 0x0C: return _state.Ports[1].SerialCtrl;
		case 0x0D: return _state.Ports[2].TxData;
		case 0x0E: return 0;
		case 0x0F: return _state.Ports[2].SerialCtrl;
	}

	return 0;
}

void MdControlManager::WriteRegister(uint8_t addr, uint8_t value)
{
	switch(addr & 0x0F) {
		case 0x01: case 0x02: case 0x03: WriteDataPort((addr & 0x0F) - 1, value); break;
		case 0x04: case 0x05: case 0x06: WriteCtrlPort((addr & 0x0F) - 4, value); break;

		//TODOMD serial port modes are not emulated
		case 0x07: _state.Ports[0].TxData = value; break;
		case 0x09: _state.Ports[0].SerialCtrl = value & 0xF8; break;
		case 0x0A: _state.Ports[1].TxData = value; break;
		case 0x0C: _state.Ports[1].SerialCtrl = value & 0xF8; break;
		case 0x0D: _state.Ports[2].TxData = value; break;
		case 0x0F: _state.Ports[2].SerialCtrl = value & 0xF8; break;
	}
}

void MdControlManager::Serialize(Serializer& s)
{
	BaseControlManager::Serialize(s);

	for(int i = 0; i < 3; i++) {
		SVI(_state.Ports[i].Data);
		SVI(_state.Ports[i].Ctrl);
		SVI(_state.Ports[i].TxData);
		SVI(_state.Ports[i].SerialCtrl);
	}

	if(!s.IsSaving()) {
		UpdateControlDevices();
	}

	for(uint8_t i = 0; i < _controlDevices.size(); i++) {
		SVI(_controlDevices[i]);
	}
}
