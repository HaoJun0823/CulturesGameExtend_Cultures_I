#pragma once
#include <cstdint>
#include <vector>

// Memory-patching toolkit (zero-dependency, Windows API only).
// All writes temporarily switch the page to PAGE_EXECUTE_READWRITE via
// VirtualProtect, then restore it after writing.
namespace Patch {

// Rewrite the instruction at the target address into a 5-byte near jump:
//   E9 <rel32>  -> jump to the cave function
// Extra bytes that are overwritten but not used by the instruction itself are
// filled with 0x90 (NOP) (nopCount >= 0).
bool WriteJmp(uintptr_t target, uintptr_t cave, int nopCount = 0);

// Directly write an arbitrary byte sequence.
bool WriteBytes(uintptr_t addr, const uint8_t* data, size_t size);

// Write a single byte / 4-byte value (little-endian).
bool WriteU8(uintptr_t addr, uint8_t v);
bool WriteU32(uintptr_t addr, uint32_t v);

// Write a pointer (32-bit address) into the target address.
bool WritePointer(uintptr_t addr, uintptr_t ptr);

// Read the current bytes at the target address.
std::vector<uint8_t> ReadBytes(uintptr_t addr, size_t size);

// Read memory at the target address.
bool ReadMemory(uintptr_t addr, void* out, size_t size);

// Write memory at the target address (any size, auto-adjusts page attributes).
bool WriteMemory(uintptr_t addr, const void* data, size_t size);

// Ensure the size bytes at the target address are writable.
bool EnsureWritable(uintptr_t addr, size_t size);

} // namespace Patch
