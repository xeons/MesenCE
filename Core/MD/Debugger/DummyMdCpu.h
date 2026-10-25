#pragma once

#include "pch.h"
#include "Debugger/DebugTypes.h"

#define DUMMYCPU
#define MdCpu DummyMdCpu
#include "MD/MdCpu.h"
#undef MdCpu
#undef DUMMYCPU
