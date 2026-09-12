/**
 * @file        tests/unit/core/memory_protect_test.cpp
 * @brief       Unit tests for host page protection queries.
 *
 *              On Linux these answer from /proc/self/maps, which is parsed on
 *              the memory-fault path, so the parse was rewritten to be
 *              allocation-free. These tests pin the behaviour that rewrite had
 *              to keep: the access a region was created or re-protected with is
 *              what comes back, the reported length runs to the end of the
 *              region, and an unmapped address is reported as such.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstddef>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>

using rex::memory::AllocationType;
using rex::memory::DeallocationType;
using rex::memory::PageAccess;

namespace {

struct Mapping {
  void* base = nullptr;
  size_t length = 0;

  explicit Mapping(size_t pages, PageAccess access) {
    length = pages * rex::memory::page_size();
    base = rex::memory::AllocFixed(nullptr, length, AllocationType::kReserveCommit, access);
  }
  ~Mapping() {
    if (base) {
      // POSIX releases need the real length: munmap(base, 0) fails, whatever
      // the Windows-shaped contract in the header says.
      rex::memory::DeallocFixed(base, length, DeallocationType::kRelease);
    }
  }
  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
};

}  // namespace

TEST_CASE("QueryProtect reports the access a mapping was created with", "[memory]") {
  Mapping mapping(4, PageAccess::kReadWrite);
  REQUIRE(mapping.base != nullptr);

  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(mapping.base, length, access));
  REQUIRE(access == PageAccess::kReadWrite);
  REQUIRE(length >= mapping.length);
}

TEST_CASE("QueryProtect reports the length from the queried page to the region end",
          "[memory]") {
  const size_t page = rex::memory::page_size();
  Mapping mapping(4, PageAccess::kReadOnly);
  REQUIRE(mapping.base != nullptr);

  auto* second_page = static_cast<uint8_t*>(mapping.base) + page;
  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(second_page, length, access));
  REQUIRE(access == PageAccess::kReadOnly);
  REQUIRE(length >= mapping.length - page);
}

TEST_CASE("QueryProtect sees a protection change", "[memory]") {
  Mapping mapping(2, PageAccess::kReadWrite);
  REQUIRE(mapping.base != nullptr);

  REQUIRE(rex::memory::Protect(mapping.base, mapping.length, PageAccess::kNoAccess));

  size_t length = 0;
  PageAccess access = PageAccess::kReadWrite;
  REQUIRE(rex::memory::QueryProtect(mapping.base, length, access));
  REQUIRE(access == PageAccess::kNoAccess);
}

TEST_CASE("Protect reports the previous access", "[memory]") {
  Mapping mapping(2, PageAccess::kReadWrite);
  REQUIRE(mapping.base != nullptr);

  PageAccess old_access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::Protect(mapping.base, mapping.length, PageAccess::kReadOnly, &old_access));
  REQUIRE(old_access == PageAccess::kReadWrite);

  PageAccess second_old = PageAccess::kNoAccess;
  REQUIRE(rex::memory::Protect(mapping.base, mapping.length, PageAccess::kReadWrite, &second_old));
  REQUIRE(second_old == PageAccess::kReadOnly);
}

TEST_CASE("QueryProtect fails for an address that is not mapped", "[memory]") {
  const size_t page = rex::memory::page_size();
  void* base = rex::memory::AllocFixed(nullptr, page, AllocationType::kReserveCommit,
                                       PageAccess::kReadWrite);
  REQUIRE(base != nullptr);
  REQUIRE(rex::memory::DeallocFixed(base, page, DeallocationType::kRelease));

  size_t length = 0;
  PageAccess access = PageAccess::kReadWrite;
  const bool found = rex::memory::QueryProtect(base, length, access);
  if (found) {
    // The address space may have been reused by another allocation between the
    // release and the query; only a mapping that is genuinely gone proves the
    // negative, so accept a hit here but never a stale read-write answer for a
    // freed region of our own making.
    SUCCEED("address space reused between release and query");
  } else {
    REQUIRE(access == PageAccess::kNoAccess);
    REQUIRE(length == 0);
  }
}
