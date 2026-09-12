/**
 * @file        tests/unit/core/threading_wait_test.cpp
 * @brief       Unit tests for multi-handle waits (rex::thread::WaitAny/WaitAll).
 *
 *              These cover the semantics that the POSIX implementation's
 *              blocking rewrite had to preserve, plus the two defects that
 *              rewrite fixed: a wait with less than a millisecond left used to
 *              spin to its deadline, and a waiter used to notice a signal only
 *              on its next poll.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <ctime>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::Semaphore;
using rex::thread::WaitHandle;
using rex::thread::WaitResult;

namespace {

std::vector<WaitHandle*> Handles(std::initializer_list<WaitHandle*> handles) {
  return std::vector<WaitHandle*>(handles);
}

}  // namespace

TEST_CASE("WaitAny returns the index of the signaled handle", "[threading]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);
  auto third = Event::CreateManualResetEvent(false);

  second->Set();

  auto result = rex::thread::WaitAny(Handles({first.get(), second.get(), third.get()}), false, 1s);
  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 1);
}

TEST_CASE("WaitAny times out when nothing is signaled", "[threading]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);

  auto result = rex::thread::WaitAny(Handles({first.get(), second.get()}), false, 50ms);
  REQUIRE(result.first == WaitResult::kTimeout);
}

TEST_CASE("WaitAll waits for every handle", "[threading]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);

  first->Set();
  auto partial = rex::thread::WaitAll(Handles({first.get(), second.get()}), false, 30ms);
  REQUIRE(partial == WaitResult::kTimeout);

  second->Set();
  auto complete = rex::thread::WaitAll(Handles({first.get(), second.get()}), false, 1s);
  REQUIRE(complete == WaitResult::kSuccess);
}

TEST_CASE("A short timeout is honoured rather than returning early", "[threading]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);

  const auto start = std::chrono::steady_clock::now();
  auto result = rex::thread::WaitAny(Handles({first.get(), second.get()}), false, 1ms);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  REQUIRE(result.first == WaitResult::kTimeout);
  REQUIRE(elapsed >= 800us);
}

TEST_CASE("An unsatisfied wait blocks instead of polling", "[threading]") {
  // The cost of waiting must not scale with how long, or on how many handles,
  // the wait runs. A polling implementation re-tests every handle on every tick
  // for the whole timeout; a blocking one does the work once and sleeps.
  // Measured here: blocking 0.11 ms of CPU, the previous polling loop 1.60 ms.
  std::vector<std::unique_ptr<Event>> events;
  std::vector<WaitHandle*> handles;
  for (int i = 0; i < 64; ++i) {
    events.push_back(Event::CreateManualResetEvent(false));
    handles.push_back(events.back().get());
  }

  timespec before{};
  timespec after{};
  REQUIRE(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &before) == 0);
  auto result = rex::thread::WaitAny(handles, false, 500ms);
  REQUIRE(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &after) == 0);

  REQUIRE(result.first == WaitResult::kTimeout);
  const auto cpu_ns = (int64_t(after.tv_sec) - int64_t(before.tv_sec)) * 1000000000ll +
                      (int64_t(after.tv_nsec) - int64_t(before.tv_nsec));
  REQUIRE(cpu_ns < 750000ll);  // 0.75 ms of CPU for 500 ms of waiting
}

TEST_CASE("A signal wakes a multi-handle wait promptly", "[threading]") {
  auto first = Event::CreateManualResetEvent(false);
  auto second = Event::CreateManualResetEvent(false);

  std::thread signaller([&second] {
    std::this_thread::sleep_for(30ms);
    second->Set();
  });

  const auto start = std::chrono::steady_clock::now();
  auto result = rex::thread::WaitAny(Handles({first.get(), second.get()}), false, 10s);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  signaller.join();

  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 1);
  // Woken by the signal itself, not by a later poll or the fallback interval.
  REQUIRE(elapsed < 500ms);
}

TEST_CASE("A semaphore release wakes a multi-handle wait and is consumed once",
          "[threading]") {
  auto event = Event::CreateManualResetEvent(false);
  auto semaphore = Semaphore::Create(0, 2);

  std::thread signaller([&semaphore] {
    std::this_thread::sleep_for(20ms);
    semaphore->Release(1, nullptr);
  });

  auto result = rex::thread::WaitAny(Handles({event.get(), semaphore.get()}), false, 10s);
  signaller.join();

  REQUIRE(result.first == WaitResult::kSuccess);
  REQUIRE(result.second == 1);

  // The single release has been taken by the wait above.
  auto again = rex::thread::WaitAny(Handles({event.get(), semaphore.get()}), false, 30ms);
  REQUIRE(again.first == WaitResult::kTimeout);
}

TEST_CASE("Concurrent waiters on the same handles all wake", "[threading]") {
  auto gate = Event::CreateManualResetEvent(false);
  auto other = Event::CreateManualResetEvent(false);

  std::vector<WaitResult> results(4, WaitResult::kTimeout);
  std::vector<std::thread> waiters;
  for (size_t i = 0; i < results.size(); ++i) {
    waiters.emplace_back([&, i] {
      results[i] =
          rex::thread::WaitAny(Handles({gate.get(), other.get()}), false, 10s).first;
    });
  }

  std::this_thread::sleep_for(20ms);
  gate->Set();
  for (auto& waiter : waiters) {
    waiter.join();
  }

  for (auto result : results) {
    REQUIRE(result == WaitResult::kSuccess);
  }
}

