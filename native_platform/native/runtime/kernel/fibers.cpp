// Stackful XAPI fibers for recompiled guest jobs. Each fiber owns a PPCContext
// so a suspended job can resume on a different guest worker thread.
#include "kernel.h"
#include "fibers.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <ucontext.h>
#include <rex/hook.h>

namespace xn {
void SetFiberContext(PPCContext* context);
namespace {
thread_local JumpBuffers thread_jump_buffers;
thread_local JumpBuffers* current_jump_buffers = &thread_jump_buffers;
struct Fiber {
  ucontext_t host{};
  PPCContext registers{};
  PPCContext* context = &registers;
  std::unique_ptr<uint8_t[]> host_stack;
  uint32_t address = 0, stack = 0, size = 0, start = 0, parameter = 0;
  GuestThread* root_thread = nullptr;
  JumpBuffers owned_jump_buffers;
  JumpBuffers* jump_buffers = &owned_jump_buffers;
  bool active = false;
};
std::mutex fiber_mutex;
std::map<uint32_t, std::unique_ptr<Fiber>> fibers;

void Activate(Fiber& fiber) {
  auto* thread = CurrentThread();
  fiber.context->r13.u64 = thread->kpcr();
  SetFiberContext(fiber.context);
  current_jump_buffers = fiber.jump_buffers;
  Store32(thread->kthread() + 0x164, fiber.address);
  Store32(thread->kthread() + kKthreadStackBase, Load32(fiber.address + 8));
  Store32(thread->kthread() + kKthreadStackLimit, Load32(fiber.address + 12));
  Store32(thread->kthread() + 0xD0, Load32(fiber.address + 4));
  Store32(thread->kpcr() + kKpcrStackBase, Load32(fiber.address + 8));
  Store32(thread->kpcr() + kKpcrStackEnd, Load32(fiber.address + 12));
}

void EnterFiber(uint32_t address) {
  // The switcher keeps the mutex held until swapcontext has finished saving
  // the outgoing stack; only then may another worker take that fiber.
  Fiber* fiber = fibers.at(address).get();
  fiber_mutex.unlock();
  auto* function = LookupFunction(fiber->start);
  if (!function) Fatal("native fiber entry is not registered");
  fiber->context->r3.u64 = fiber->parameter;
  function(*fiber->context, GuestBase());
  Fatal("native fiber start routine returned");
}

uint32_t Convert(PPCContext& ctx, uint32_t parameter) {
  auto* thread = CurrentThread();
  if (!thread) Fatal("ConvertThreadToFiber requires a guest thread");
  if (Load32(thread->kthread() + 0x164)) { thread->SetLastError(0x500); return 0; }
  auto fiber = std::make_unique<Fiber>();
  fiber->address = K().arena.SystemAlloc(0xA50, 16);
  fiber->root_thread = thread;
  fiber->jump_buffers = &thread_jump_buffers;
  fiber->context = &ctx;
  fiber->active = true;
  Store32(fiber->address, parameter);
  Store32(fiber->address + 4, Load32(thread->kthread() + kKthreadStackBase));
  Store32(fiber->address + 8, Load32(thread->kthread() + kKthreadStackBase));
  Store32(fiber->address + 12, Load32(thread->kthread() + kKthreadStackLimit));
  Store64(fiber->address + 0x30, ctx.r1.u64);
  auto address = fiber->address;
  std::lock_guard lock(fiber_mutex);
  Activate(*fiber);
  fibers.emplace(address, std::move(fiber));
  return address;
}
}  // namespace

JumpBuffers& CurrentJumpBuffers() { return *current_jump_buffers; }

XN_IMPORT_RAW(rexcrt_ConvertThreadToFiber) { ctx.r3.u64 = Convert(ctx, ctx.r3.u32); }
XN_IMPORT_RAW(rexcrt_ConvertFiberToThread) {
  std::lock_guard lock(fiber_mutex);
  auto* thread = CurrentThread();
  auto address = Load32(thread->kthread() + 0x164);
  auto found = fibers.find(address);
  if (found == fibers.end() || found->second->root_thread != thread) {
    thread->SetLastError(0x501); ctx.r3.u64 = 0; return;
  }
  Store32(thread->kthread() + 0x164, 0);
  SetFiberContext(nullptr);
  current_jump_buffers = &thread_jump_buffers;
  K().arena.SystemFree(address);
  fibers.erase(found);
  ctx.r3.u64 = 1;
}

XN_IMPORT_RAW(rexcrt_CreateFiber) {
  auto fiber = std::make_unique<Fiber>();
  fiber->size = std::max(0x4000u, (ctx.r3.u32 + 0xFFFu) & ~0xFFFu);
  if (!ctx.r3.u32) fiber->size = 0x10000;
  fiber->start = ctx.r4.u32;
  fiber->parameter = ctx.r5.u32;
  fiber->stack = K().arena.AllocVirtual(fiber->size, 0x1000, true);
  if (!fiber->stack) { CurrentThread()->SetLastError(8); ctx.r3.u64 = 0; return; }
  fiber->address = K().arena.SystemAlloc(0xA50, 16);
  fiber->registers = ctx;
  fiber->registers.r1.u64 = fiber->stack + fiber->size - 0x50;
  Store32(fiber->address, fiber->parameter);
  Store32(fiber->address + 4, fiber->stack + fiber->size);
  Store32(fiber->address + 8, fiber->stack + fiber->size);
  Store32(fiber->address + 12, fiber->stack);
  Store64(fiber->address + 0x30, fiber->registers.r1.u64);
  size_t host_size = std::max<size_t>(fiber->size, 1024 * 1024);
  // Reserve the stack without touching every page in a large job pool.
  fiber->host_stack.reset(new uint8_t[host_size]);
  if (getcontext(&fiber->host)) Fatal("getcontext failed");
  fiber->host.uc_stack.ss_sp = fiber->host_stack.get();
  fiber->host.uc_stack.ss_size = host_size;
  fiber->host.uc_link = nullptr;
  makecontext(&fiber->host, reinterpret_cast<void(*)()>(EnterFiber), 1, fiber->address);
  auto address = fiber->address;
  { std::lock_guard lock(fiber_mutex); fibers.emplace(address, std::move(fiber)); }
  ctx.r3.u64 = address;
}

XN_IMPORT_RAW(rexcrt_SwitchToFiber) {
  uint32_t target_address = ctx.r3.u32;
  auto* thread = CurrentThread();
  uint32_t source_address = Load32(thread->kthread() + 0x164);
  if (!source_address) source_address = Convert(ctx, 0);
  if (source_address == target_address) return;
  fiber_mutex.lock();
  auto source = fibers.find(source_address), target = fibers.find(target_address);
  if (source == fibers.end() || target == fibers.end() || target->second->active ||
      (target->second->root_thread && target->second->root_thread != thread)) {
    fiber_mutex.unlock(); Fatal("invalid native fiber switch");
  }
  Store64(source_address + 0x30, ctx.r1.u64);
  source->second->active = false;
  target->second->active = true;
  Activate(*target->second);
  if (swapcontext(&source->second->host, &target->second->host)) Fatal("swapcontext failed");
  fiber_mutex.unlock();
}

XN_IMPORT_RAW(rexcrt_DeleteFiber) {
  auto address = ctx.r3.u32;
  std::lock_guard lock(fiber_mutex);
  auto found = fibers.find(address);
  if (found == fibers.end()) return;
  if (found->second->active) Fatal("self-deleting native fibers are not implemented");
  if (found->second->stack) K().arena.FreeVirtual(found->second->stack, nullptr);
  K().arena.SystemFree(address);
  fibers.erase(found);
}
}  // namespace xn
