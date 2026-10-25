#pragma once
#include "pch.h"
#include "Debugger/IAssembler.h"

class LabelManager;

class MdAssembler final : public IAssembler
{
private:
	LabelManager* _labelManager;

public:
	MdAssembler(LabelManager* labelManager)
	{
		_labelManager = labelManager;
	}

	virtual ~MdAssembler() {}

	uint32_t AssembleCode(string code, uint32_t startAddress, int16_t* assembledCode)
	{
		//TODOMD 68000 assembler
		return 0;
	}
};
