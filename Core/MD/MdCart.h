#pragma once
#include "pch.h"
#include "MD/MdTypes.h"
#include "Shared/SettingTypes.h"
#include "Utilities/ISerializable.h"

class Emulator;
class MdConsole;

class MdCart final : public ISerializable
{
private:
	static constexpr uint32_t DefaultSaveRamStart = 0x200000;
	static constexpr uint32_t DefaultSaveRamEnd = 0x20FFFF;

	Emulator* _emu = nullptr;
	MdConsole* _console = nullptr;

	uint8_t* _prgRom = nullptr;
	uint32_t _prgRomSize = 0;
	uint32_t _prgRomMask = 0;

	uint8_t* _saveRam = nullptr;
	uint32_t _saveRamSize = 0;
	uint32_t _saveRamStart = 0;
	uint32_t _saveRamEnd = 0;
	MdCartRamType _saveRamType = MdCartRamType::None;
	bool _hasBattery = false;
	bool _saveRamUsed = false;

	bool _hasSegaMapper = false;
	bool _saveRamEnabled = false;
	bool _saveRamWriteProtected = false;
	uint8_t _romBanks[8] = {};

	string _headerRegions;

	void ParseHeader();
	int32_t GetSaveRamOffset(uint32_t addr);

public:
	MdCart(Emulator* emu, MdConsole* console, vector<uint8_t>& romData);
	~MdCart();

	static void DeinterleaveSmd(vector<uint8_t>& romData);

	uint8_t* GetPrgRom() { return _prgRom; }
	uint32_t GetPrgRomSize() { return _prgRomSize; }
	uint8_t* GetSaveRam() { return _saveRam; }
	uint32_t GetSaveRamSize() { return _saveRamSize; }

	//Returns the ROM data that should be mapped to the given 64KB page of the 68000's address space
	uint8_t* GetRomPage(uint8_t page);

	//True when the 64KB page contains cart RAM (or needs special handling) and must go through ReadRam/WriteRam
	bool IsSpecialPage(uint8_t page);

	uint8_t ReadRam(uint32_t addr, bool& handled);
	void WriteRam(uint32_t addr, uint8_t value);

	//Registers at $A130F1-$A130FF
	void WriteRegister(uint32_t addr, uint8_t value);

	ConsoleRegion GetHeaderRegion();

	void LoadBattery();
	void SaveBattery();

	void Serialize(Serializer& s) override;
};
