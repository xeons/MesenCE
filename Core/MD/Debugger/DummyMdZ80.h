#pragma once

#include "pch.h"
#include "Debugger/DebugTypes.h"

#define DUMMYCPU
#define MdZ80Cpu DummyMdZ80
#include "MD/MdZ80.h"
#undef MdZ80Cpu
#undef DUMMYCPU
