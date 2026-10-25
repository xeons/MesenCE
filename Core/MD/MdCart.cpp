#include "pch.h"
#include "MD/MdCart.h"
#include "MD/MdConsole.h"
#include "Shared/Emulator.h"
#include "Shared/BatteryManager.h"
#include "Shared/MessageManager.h"
#include "Utilities/Serializer.h"

MdCart::MdCart(Emulator* emu, MdConsole* console, vector<uint8_t>& romData)
{
	_emu = emu;
	_console = console;

	//Pad the ROM to a multiple of 64KB, to simplify mapping
	uint32_t size = std::max<uint32_t>(0x10000, ((uint32_t)romData.size() + 0xFFFF) & ~0xFFFF);
	_prgRomSize = size;
	_prgRom = new uint8_t[size];
	memset(_prgRom, 0xFF, size);
	memcpy(_prgRom, romData.data(), romData.size());
	_emu->RegisterMemory(MemoryType::MdPrgRom, _prgRom, _prgRomSize);

	for(int i = 0; i < 8; i++) {
		_romBanks[i] = i;
	}

	ParseHeader();

	if(_saveRamSize > 0) {
		_saveRam = new uint8_t[_saveRamSize];
		memset(_saveRam, 0xFF, _saveRamSize);
		_emu->RegisterMemory(MemoryType::MdSaveRam, _saveRam, _saveRamSize);
		LoadBattery();
	}
}

MdCart::~MdCart()
{
	delete[] _prgRom;
	delete[] _saveRam;
}

void MdCart::DeinterleaveSmd(vector<uint8_t>& romData)
{
	//SMD dumps have a 512-byte header, followed by 16KB blocks
	//Each block contains the odd bytes in its first half, and the even bytes in its second half
	vector<uint8_t> output;
	output.reserve(romData.size() - 0x200);

	for(size_t block = 0x200; block + 0x4000 <= romData.size(); block += 0x4000) {
		for(int i = 0; i < 0x2000; i++) {
			output.push_back(romData[block + 0x2000 + i]);
			output.push_back(romData[block + i]);
		}
	}

	romData = output;
}

void MdCart::ParseHeader()
{
	auto readLong = [this](uint32_t addr) -> uint32_t {
		return (_prgRom[addr] << 24) | (_prgRom[addr + 1] << 16) | (_prgRom[addr + 2] << 8) | _prgRom[addr + 3];
	};

	_headerRegions = string((char*)_prgRom + 0x1F0, 3);

	string systemType = string((char*)_prgRom + 0x100, 16);
	_hasSegaMapper = _prgRomSize > 0x400000 || systemType.find("SEGA SSF") != string::npos;

	if(_prgRom[0x1B0] == 'R' && _prgRom[0x1B1] == 'A') {
		uint8_t ramType = _prgRom[0x1B2];
		uint32_t start = readLong(0x1B4) & 0xFFFFFF;
		uint32_t end = readLong(0x1B8) & 0xFFFFFF;

		if(_prgRom[0x1B3] == 0x40) {
			//TODOMD serial EEPROM carts are not supported yet
			MessageManager::Log("[MD] Serial EEPROM save type is not supported.");
			return;
		}

		if(end < start || end - start > 0x10000) {
			start = MdCart::DefaultSaveRamStart;
			end = MdCart::DefaultSaveRamEnd;
		}

		_hasBattery = (ramType & 0x40) != 0;
		switch((ramType >> 3) & 0x03) {
			case 2: _saveRamType = MdCartRamType::Sram8BitEven; break;
			case 3: _saveRamType = MdCartRamType::Sram8BitOdd; break;
			default: _saveRamType = MdCartRamType::Sram16Bit; break;
		}

		if(_saveRamType == MdCartRamType::Sram16Bit) {
			_saveRamSize = end - start + 1;
		} else {
			_saveRamSize = ((end - start) >> 1) + 1;
		}
		_saveRamStart = start;
		_saveRamEnd = end;
	} else if(_prgRomSize <= 0x200000) {
		//No save RAM declared in the header - map 64KB of RAM at $200000 anyway, but only save it if it gets used
		_saveRamType = MdCartRamType::Sram16Bit;
		_saveRamStart = MdCart::DefaultSaveRamStart;
		_saveRamEnd = MdCart::DefaultSaveRamEnd;
		_saveRamSize = _saveRamEnd - _saveRamStart + 1;
		_hasBattery = true;
		_saveRamUsed = false;
		_saveRamEnabled = true;
		return;
	}

	_saveRamUsed = true;

	//Save RAM is enabled by default when it doesn't overlap with ROM, otherwise $A130F1 needs to be written to
	_saveRamEnabled = _saveRamStart >= std::min<uint32_t>(_prgRomSize, 0x400000);
}

uint8_t* MdCart::GetRomPage(uint8_t page)
{
	uint32_t offset;
	if(_hasSegaMapper && page < 0x40) {
		offset = (_romBanks[page >> 3] * 0x80000) + ((page & 0x07) << 16);
	} else {
		offset = page << 16;
	}
	return _prgRom + (offset % _prgRomSize);
}

bool MdCart::IsSpecialPage(uint8_t page)
{
	return _saveRamSize > 0 && page >= (_saveRamStart >> 16) && page <= (_saveRamEnd >> 16);
}

int32_t MdCart::GetSaveRamOffset(uint32_t addr)
{
	if(!_saveRamEnabled || addr < _saveRamStart || addr > _saveRamEnd) {
		return -1;
	}

	switch(_saveRamType) {
		default:
		case MdCartRamType::None: return -1;
		case MdCartRamType::Sram8BitOdd: return (addr & 0x01) ? (int32_t)((addr - _saveRamStart) >> 1) : -1;
		case MdCartRamType::Sram8BitEven: return (addr & 0x01) ? -1 : (int32_t)((addr - _saveRamStart) >> 1);
		case MdCartRamType::Sram16Bit: return (int32_t)(addr - _saveRamStart);
	}
}

uint8_t MdCart::ReadRam(uint32_t addr, bool& handled)
{
	int32_t offset = GetSaveRamOffset(addr);
	if(offset >= 0 && (uint32_t)offset < _saveRamSize) {
		handled = true;
		return _saveRam[offset];
	}

	handled = false;
	return 0;
}

void MdCart::WriteRam(uint32_t addr, uint8_t value)
{
	if(_saveRamWriteProtected) {
		return;
	}

	int32_t offset = GetSaveRamOffset(addr);
	if(offset >= 0 && (uint32_t)offset < _saveRamSize) {
		_saveRam[offset] = value;
		_saveRamUsed = true;
	}
}

void MdCart::WriteRegister(uint32_t addr, uint8_t value)
{
	switch(addr & 0xFF) {
		case 0xF1:
			if(_saveRamSize > 0) {
				_saveRamEnabled = (value & 0x01) != 0;
				_saveRamWriteProtected = (value & 0x02) != 0;
			}
			break;

		case 0xF3: case 0xF5: case 0xF7: case 0xF9: case 0xFB: case 0xFD: case 0xFF:
			if(_hasSegaMapper) {
				_romBanks[((addr & 0xFF) - 0xF1) >> 1] = value & 0x3F;
			}
			break;
	}
}

ConsoleRegion MdCart::GetHeaderRegion()
{
	if(_headerRegions.find('U') != string::npos) {
		return ConsoleRegion::Ntsc;
	} else if(_headerRegions.find('J') != string::npos) {
		return ConsoleRegion::NtscJapan;
	} else if(_headerRegions.find('E') != string::npos) {
		return ConsoleRegion::Pal;
	}

	//New-style region code: a single hex digit (bit 0 = Japan, bit 2 = Americas, bit 3 = Europe)
	char c = (char)std::toupper(_headerRegions[0]);
	int regions = -1;
	if(c >= '0' && c <= '9') {
		regions = c - '0';
	} else if(c >= 'A' && c <= 'F') {
		regions = c - 'A' + 10;
	}

	if(regions > 0) {
		if(regions & 0x04) {
			return ConsoleRegion::Ntsc;
		} else if(regions & 0x01) {
			return ConsoleRegion::NtscJapan;
		} else if(regions & 0x08) {
			return ConsoleRegion::Pal;
		}
	}

	return ConsoleRegion::Ntsc;
}

void MdCart::LoadBattery()
{
	if(_hasBattery && _saveRamSize > 0) {
		vector<uint8_t> saveData = _emu->GetBatteryManager()->LoadBattery(".sav");
		if(saveData.size() > 0) {
			memcpy(_saveRam, saveData.data(), std::min<size_t>(saveData.size(), _saveRamSize));
			_saveRamUsed = true;
		}
	}
}

void MdCart::SaveBattery()
{
	if(_hasBattery && _saveRamSize > 0 && _saveRamUsed) {
		_emu->GetBatteryManager()->SaveBattery(".sav", _saveRam, _saveRamSize);
	}
}

void MdCart::Serialize(Serializer& s)
{
	SVArray(_saveRam, _saveRamSize);
	SV(_saveRamEnabled);
	SV(_saveRamWriteProtected);
	SV(_saveRamUsed);
	SVArray(_romBanks, 8);
}
