/**
 * @file        rex/graphics/present_stats.h
 * @brief       Guest present (VdSwap) counter, defined in the runtime so both
 *              the GPU plugin (which increments it) and the UI overlay (which
 *              reads it for the on-screen FPS) resolve the same symbol. The
 *              plugin links the runtime, so the definition must live here, not
 *              in the plugin.
 */
#pragma once
#include <cstdint>
namespace rex::graphics {
// Called once per guest present by the command processor.
void NotifyGuestPresent();
// Total guest presents so far; the overlay samples it over time for a rate.
uint64_t GuestPresentCount();
}  // namespace rex::graphics
