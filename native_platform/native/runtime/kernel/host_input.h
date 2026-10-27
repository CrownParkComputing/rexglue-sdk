#pragma once
namespace xn {
void SetHostInputCaptured(bool captured);
void SendHostStartButton();
// Optional title work deferred to the next guest-side input poll.
void SetGuestInputPollCallback(void (*callback)());
}
