#include "pch.h"
#include "MD/Debugger/MdZ80DisUtils.h"
#include "MD/Debugger/DummyMdZ80.h"
#include "MD/MdConsole.h"
#include "MD/MdTypes.h"
#include "SMS/Debugger/SmsDisUtils.h"
#include "Debugger/DisassemblyInfo.h"

EffectiveAddressInfo MdZ80DisUtils::GetEffectiveAddress(DisassemblyInfo& info, MdConsole* console, MdZ80State& state)
{
	bool isJump = SmsDisUtils::IsUnconditionalJump(info.GetOpCode()) || SmsDisUtils::IsConditionalJump(info.GetOpCode());
	if(isJump) {
		//For jumps, show no address/value
		return {};
	}

	SmsOpInfo smsOp = SmsDisUtils::GetSmsOpInfo(info);
	if(smsOp.IsEdPrefix && smsOp.ByteCode[0] >= 0xB0 && smsOp.ByteCode[0] <= 0xBF) {
		//Don't show address/value for instructions that repeat (LDIR, etc.)
		return {};
	} else if(!smsOp.IsCbPrefix && !smsOp.IsEdPrefix) {
		switch(smsOp.ByteCode[0]) {
			case 0xC1:
			case 0xD1:
			case 0xE1:
			case 0xF1:
			case 0xC5:
			case 0xD5:
			case 0xE5:
			case 0xF5:
				//Don't show address/value for push/pop
				return {};
		}
	}

	DummyMdZ80 dummyCpu;
	dummyCpu.Init(console->GetEmulator(), console, console->GetMemoryManager());
	dummyCpu.SetDummyState(state);
	dummyCpu.Exec();

	uint32_t count = dummyCpu.GetOperationCount();
	for(int i = count - 1; i > 0; i--) {
		MemoryOperationInfo opInfo = dummyCpu.GetOperationInfo(i);
		if(opInfo.Type != MemoryOperationType::ExecOperand && opInfo.Type != MemoryOperationType::DummyRead) {
			MemoryOperationInfo prevOpInfo = dummyCpu.GetOperationInfo(i - 1);
			EffectiveAddressInfo result;
			if(prevOpInfo.Type == opInfo.Type && prevOpInfo.Address == opInfo.Address - 1) {
				//For 16-bit read/writes, return the first address
				result.Address = prevOpInfo.Address;
				result.Type = prevOpInfo.MemType;
				result.ValueSize = 2;
			} else {
				result.Address = opInfo.Address;
				result.Type = opInfo.MemType;
				result.ValueSize = 1;
			}

			result.ShowAddress =
				(strstr(smsOp.Op, "(") != nullptr && strstr(smsOp.Op, "(a)") == nullptr && strstr(smsOp.Op, "(p)") == nullptr) ||
				strstr(smsOp.Op, "x") != nullptr ||
				(smsOp.HlType != HlRegType::HL && strstr(smsOp.Op, "v") != nullptr) ||
				(smsOp.IsEdPrefix && smsOp.ByteCode[0] >= 0xA0 && smsOp.ByteCode[0] <= 0xAF);
			return result;
		}
	}

	return {};
}
