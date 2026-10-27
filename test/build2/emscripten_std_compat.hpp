#pragma once

// Emscripten's libc++ does not ship the importable std.compat module. The
// build2 wasm configuration force-includes the standard headers used by the
// portable module interfaces instead.
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
