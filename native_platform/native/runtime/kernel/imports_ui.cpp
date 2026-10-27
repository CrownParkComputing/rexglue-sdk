// XAM UI blades, and the inactivity timer that sits behind the same wrappers.
//
// On a console every one of these hands control to the dashboard, draws
// something over the running title, and returns what the player chose. This
// runtime has no dashboard, so each one answers the way a console answers when
// the player dismisses it without choosing: success, with the output buffer
// written to "nothing was selected". The keyboard blade is the exception: a
// title's create-profile flow cannot finish while its name entry reports
// "dismissed", so that one answers with the prefilled name (see below). That
// is not the same as a stub. Three of these have observable consequences if
// they answer wrongly:
//
//   XamShowDirtyDiscErrorUI  must NOT return. On hardware it is terminal - the
//     console shows the disc-error screen and the title is gone. Boom Boom
//     Rocket's asset loaders spawn a thread whose entire body is
//     "li r3,0; b XamShowDirtyDiscErrorUI" (0x82076C78), and the xapilib
//     wrapper that follows it calls XamLoaderLaunchTitle(0, 0) - a reboot to
//     the dashboard. Returning would resume a title that has already decided
//     it cannot read its own data.
//   XamShowSigninUI / XamShowAchievementsUI / XamShowMarketplaceUI are reached
//     from menu entries. Returning success with nothing shown is what HANDOVER
//     3.3 records as a live defect in Geometry Wars 1: "a menu entry that does
//     nothing is exactly what a player reports". We answer, and we say so in
//     the log, so the cause is one grep away rather than a mystery.
//   XamUserCheckPrivilege writes its result through a pointer. The guest's own
//     not-signed-in fallback (0x820E76A8) writes 0 there before returning, so
//     a caller that only reads the buffer is normal for this API.
//
// Arities here were confirmed against Boom Boom Rocket's own call sites, not
// copied from a signature: see docs/boomboomrocket/imports.json. The one that
// matters is XamShowGamerCardUIForXUID, whose XUID is a 64-bit guest argument
// and therefore occupies ONE register (rex/hook.h, rule 1) - the caller loads
// it with a single `ld` at 0x8209F274.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

#include "xtypes.h"

#include <rex/hook.h>

#include "kernel.h"
#include "local_profile.h"

using namespace xn;

namespace {
// A blade that was dismissed without a choice. XAM reports success; the
// "nothing happened" part is in the output buffer, never in the status.
uint32_t Dismissed(uint32_t overlapped, uint32_t result = X_ERROR_SUCCESS) {
  if (!overlapped) return result;
  CompleteOverlapped(overlapped, result);
  return X_ERROR_IO_PENDING;
}

// A NUL-terminated UTF-16 guest string, flattened to ASCII for the log. The
// arena maps the whole guest space, so a malformed pointer reads zeros rather
// than faulting; non-ASCII characters become '?' instead of failing to decode.
std::string GuestWideAsAscii(uint32_t address, uint32_t max_chars = 256) {
  std::string out;
  if (!address) return out;
  for (uint32_t i = 0; i < max_chars; i++) {
    const uint16_t c = Load16(address + i * 2);
    if (!c) break;
    out.push_back(c < 0x80 ? char(c) : '?');
  }
  return out;
}
}  // namespace

// XamShowSigninUI(pane_count, flags). Boom Boom Rocket calls it three ways:
// (1, 2) and (1, 0) from 0x8209E3E8/0x8209E540 after finding user 0 not signed
// in the way it wanted, and (user, 0) from 0x8209B800.
u32 XamShowSigninUI_entry(u32 pane_count, u32 flags) {
  REXKRNL_WARN("XamShowSigninUI(panes={}, flags={:#x}): no dashboard; the local profile stays as "
               "it is and the title sees no sign-in change",
               pane_count, flags);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamShowSigninUI, XamShowSigninUI_entry)

// XamShowAchievementsUI(user_index, unused). The xapilib wrapper at 0x820E73B8
// forces the second argument to 0.
u32 XamShowAchievementsUI_entry(u32 user_index, u32 unused) {
  REXKRNL_WARN("XamShowAchievementsUI(user={}): no dashboard, so nothing is drawn. The launcher "
               "keeps the achievement list; this menu entry cannot show it.",
               user_index);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamShowAchievementsUI, XamShowAchievementsUI_entry)

// XamShowGamerCardUIForXUID(user_index, xuid, overlapped). 64-bit xuid in one
// register. The wrapper at 0x820E73B0 forces overlapped to 0.
u32 XamShowGamerCardUIForXUID_entry(u32 user_index, u64 xuid, u32 overlapped) {
  REXKRNL_WARN("XamShowGamerCardUIForXUID(user={}, xuid={:#016x}): no dashboard", user_index, xuid);
  return Dismissed(overlapped);
}
XN_IMPORT(__imp__XamShowGamerCardUIForXUID, XamShowGamerCardUIForXUID_entry)

// XamShowMarketplaceUI(user_index, type, offering(64-bit), category, ..., overlapped).
// Six registers at the thunk: the wrapper at 0x820E73C0 shifts its caller's
// fourth argument into r7 and passes r6 = -1, r8 = 0. Boom Boom Rocket reaches
// it from 0x8209E3E8 with (user, 1, 0, -1), i.e. "show the offer list for this
// title" - which would sell exactly the DLC pack the user already owns.
u32 XamShowMarketplaceUI_entry(u32 user_index, u32 type, u32 arg3, u32 arg4, u32 arg5,
                               u32 overlapped) {
  REXKRNL_WARN("XamShowMarketplaceUI(user={}, type={}, {:#x}, {:#x}, {:#x}): no marketplace. "
               "Downloadable content has to be supplied locally instead.",
               user_index, type, arg3, arg4, arg5);
  return Dismissed(overlapped);
}
XN_IMPORT(__imp__XamShowMarketplaceUI, XamShowMarketplaceUI_entry)

// XamShowDirtyDiscErrorUI(user_index). Terminal on hardware; terminal here.
// Parking the calling thread rather than returning matches both the console and
// what the xapilib wrapper does next (XamLoaderLaunchTitle, a reboot), and it
// leaves the rest of the title running long enough for the log to be read.
XN_IMPORT_RAW(__imp__XamShowDirtyDiscErrorUI) {
  GuestThread* thread = CurrentThread();
  // PGR4's generated wrapper sub_8267B3C8 has a 0x60-byte frame and saves
  // its incoming LR at new_sp + 0x58. This lets the fatal-path log identify
  // the guest routine that elected to show the disc-error UI, not just the
  // wrapper's own import call site.
  uint32_t saved_caller = Load32(ctx.r1.u32 + 0x58);
  REXKRNL_ERROR("XamShowDirtyDiscErrorUI(user={}, lr={:#010x}, saved_caller={:#010x}, "
                "tid={:#x}, thread_start={:#010x}, sp={:#010x}): title reported an "
                "unrecoverable read error and requested the terminal disc-error screen",
                ctx.r3.u32, uint32_t(ctx.lr), saved_caller,
                thread ? thread->thread_id() : 0,
                thread ? thread->params().start_address : 0, ctx.r1.u32);
  K().title_running = false;
  while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}

// XamUserCheckPrivilege(user_index, privilege, result*). Offline: no privilege
// is granted. The output buffer is written on every path, including the
// invalid-user one, because the guest reads it whether or not it checks the
// status (its own fallback at 0x820E76A8 does exactly that).
u32 XamUserCheckPrivilege_entry(u32 user_index, u32 privilege, mapped_u32 result) {
  if (result) *result = 0;
  if (!result) return X_ERROR_INVALID_PARAMETER;
  if (user_index != 0 && user_index != 0xFF) return X_ERROR_NO_SUCH_USER;
  REXKRNL_DEBUG("XamUserCheckPrivilege(user={}, privilege={}) -> not granted (offline)",
                user_index, privilege);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege_entry)

// The inactivity timer. xapilib calls both from one wrapper (0x820E7CA8): reset
// first when its argument is 0, then enable/disable. A console uses this to
// stop the screensaver and the idle sign-out while a song is playing. There is
// nothing underneath either here, and nothing observable follows from that, so
// these are the two genuine stubs in this file - but they still return success,
// because the wrapper turns a negative status into error 0x65B for its caller.
u32 XamEnableInactivityProcessing_entry(u32 user_index, u32 enable) {
  REXKRNL_DEBUG("XamEnableInactivityProcessing(user={}, enable={})", user_index, enable);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamEnableInactivityProcessing, XamEnableInactivityProcessing_entry)

u32 XamResetInactivity_entry(u32 unknown) {
  REXKRNL_DEBUG("XamResetInactivity({})", unknown);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamResetInactivity, XamResetInactivity_entry)

// OutRun Online Arcade's xapilib passes these through bare thunks (0x8227DF38
// branches straight to the import; 0x8227DEF8 marshals its caller's stack
// arguments and tail-calls), so the guest's argument count is not visible at
// the call site. They are online-friends / review / invite blades: nothing in
// an offline menu or race reaches them. Answer as the immediately-dismissed
// blade through the raw path - success, no memory written, because marshalling
// a guessed arity could write an output buffer to the wrong guest address.
// Revisit with real arities before bringing up OutRun's online menus.
XN_IMPORT_RAW(__imp__XamShowCustomPlayerListUI) {
  REXKRNL_WARN("XamShowCustomPlayerListUI: no dashboard; reporting an immediately dismissed "
               "player-list blade (offline runtime)");
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

// XamShowKeyboardUI(user_index, flags, title_text, description_text,
//                   default_text, result_text, cch_result_text, overlapped).
//
// The arity is the documented XDK one, confirmed against the call sites in
// Burnout Revenge: the xapilib thunk at 0x825AF678 tail-calls the import from
// sub_8210D978 and sub_82111400, which hand it r5 = the title, r6 = the
// description, r7 = the prefilled text, r8 = the result text buffer,
// r9 = cchResultText and r10 = the XOVERLAPPED. The blade cannot be drawn, so
// it answers what a player pressing Done would: the local profile's gamertag,
// written back as UTF-16 with the overlapped completed. Reporting "dismissed"
// instead leaves a create-profile flow that can never finish - Burnout
// Revenge re-opens the prompt every time its profile UI comes back around and
// never creates the player file.
XN_IMPORT_RAW(__imp__XamShowKeyboardUI) {
  const uint32_t default_text = ctx.r7.u32;
  const uint32_t result_text = ctx.r8.u32;
  const uint32_t cch_result_text = ctx.r9.u32;  // WCHARs, terminator included
  const uint32_t overlapped = ctx.r10.u32;

  // The answer is the local profile's gamertag, not the guest's prefilled text:
  // one user exists here, XamUserGetName reports the same string, and the
  // title's save then links to the launcher profile by name as well as by XUID.
  // The prefilled text is logged but not used because it is not always a name -
  // Burnout Revenge passes its prompt, "Enter the name for your player file",
  // there, and echoing it would store the prompt as the player's name.
  std::u16string name;
  for (char c : LocalUser().gamertag()) name.push_back(char16_t(uint8_t(c)));
  if (result_text && cch_result_text > 1) {
    const uint32_t count = std::min<uint32_t>(cch_result_text - 1, uint32_t(name.size()));
    for (uint32_t i = 0; i < count; i++) Store16(result_text + i * 2, name[i]);
    Store16(result_text + count * 2, 0);
  }
  REXKRNL_WARN("XamShowKeyboardUI(user={}, flags={:#x}, title=\"{}\", description=\"{}\", "
               "default=\"{}\"): no dashboard; answering Done with \"{}\" ({} of {} chars)",
               ctx.r3.u32, ctx.r4.u32, GuestWideAsAscii(ctx.r5.u32, 64),
               GuestWideAsAscii(ctx.r6.u32, 64), GuestWideAsAscii(default_text, 64),
               LocalUser().gamertag(), uint32_t(name.size()), cch_result_text);
  ctx.r3.u64 = Dismissed(overlapped, X_ERROR_SUCCESS);
}

XN_IMPORT_RAW(__imp__XamShowPlayerReviewUI) {
  REXKRNL_WARN("XamShowPlayerReviewUI: no dashboard; reporting an immediately dismissed review blade");
  ctx.r3.u64 = X_ERROR_SUCCESS;
}

XN_IMPORT_RAW(__imp__XamShowGameInviteUI) {
  REXKRNL_WARN("XamShowGameInviteUI: no dashboard; reporting an immediately dismissed invite blade");
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
