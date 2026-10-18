#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <atomic>
#include <vector>
#include <functional>
#include <memory>

// 注意顺序：CommonLibF4 的 REX/W32/BASE.h 会在「Windows.h 已经被包含」时
// 主动 #error（它要用自己的 Win32 绑定）。所以 Windows/D3D 头必须放在它之后。
#include "F4SE/F4SE.h"
#include "RE/Fallout.h"
// BSGeometry.h / BSTriShape.h 只在 CommonLibF4 的 master 里有（dev 分支没有），
// CI 的 overlay 是「dev 覆盖 master」，master 独有的头文件会保留在磁盘上，
// 所以构建机上这两个头可用；本地 clone 是 dev，看不到它们，属正常。
#include "RE/Bethesda/BSGeometry.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <intrin.h>
