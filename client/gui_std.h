// The headers in front of gui_state.h's imports, on their own so MSVC can precompile them (gui_pch.cpp). An
// import can't be in a precompiled header, so this stops where the imports start. Each gui_*.cpp includes this first
#pragma once

#include <cstdint>
#include <type_traits>
#include <fan/utils/utility.h>
#include <fan/types/dme.h>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <exception>
#include <coroutine>
#include <string>
#include <array>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <future>
#include <queue>
#include <deque>
#include <memory>
#include <cmath>
#include <fstream>
#include <sstream>
#include <ctime>
#include <filesystem>
#include <random>
#include <cstdio>
#include <cctype>
#include <stdexcept>
#include <expected>
#include <charconv>
