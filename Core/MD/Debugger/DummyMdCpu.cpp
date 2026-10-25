#include "pch.h"
#include "MD/Debugger/DummyMdCpu.h"

#define DUMMYCPU
#define MdCpu DummyMdCpu
#include "MD/MdCpu.cpp"
#include "MD/MdCpu.Instructions.cpp"
#undef MdCpu
#undef DUMMYCPU
