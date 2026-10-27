#pragma once
#include <csetjmp>
#include <cstdint>
#include <unordered_map>

namespace xn {
using JumpBuffers = std::unordered_map<uint32_t, jmp_buf>;
// A stack's exception/longjmp targets follow the fiber when a job migrates.
JumpBuffers& CurrentJumpBuffers();
}
