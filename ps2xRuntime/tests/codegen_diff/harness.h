// Shared by the generated chunk_*.cpp translation units and test.cpp.
// Must be the first include: it exposes PS2Runtime privates so the test can
// hand generated code a fake runtime object with a finite checkpoint budget.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <queue>
#include <list>
#include <array>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <optional>
#include <variant>
#include <chrono>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <utility>
#include <stdexcept>
#include <filesystem>
#include <bit>
#include <cfenv>
#include <string_view>
#include <span>
#include <random>
#include <limits>
#include <tuple>
#include <type_traits>
#include <cassert>
#include <cstddef>
#include <cinttypes>
#include <future>
#include <numeric>

#define private public
#define protected public
#include "ps2_runtime.h"
#undef private
#undef protected
#include "ps2_runtime_macros.h"

struct FuncEntry
{
    const char *name;
    void (*fn)(uint8_t *, R5900Context *, PS2Runtime *);
};
struct ChunkRef
{
    const FuncEntry *entries;
    const int *count;
};
