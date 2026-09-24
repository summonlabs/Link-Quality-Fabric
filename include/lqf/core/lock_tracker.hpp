// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef LQF_CORE_LOCK_TRACKER_HPP
#define LQF_CORE_LOCK_TRACKER_HPP

#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/export.hpp"

namespace lqf::locks {

enum class LockMode : u8 { Shared, Exclusive };

enum class ViolationKind : u8 { None, Reentrancy, OrderInversion };

struct LockViolation {
  ViolationKind kind{ViolationKind::None};
  std::string acquiring{};
  std::string held{};
};

// Runtime lock audit. Two defects are detected before they can deadlock:
//   * re-entrancy - the same thread acquiring a lock it already holds, which
//     for a std::shared_mutex is an unconditional self-deadlock;
//   * order inversion - acquiring B while holding A when A -> B was never the
//     established order, i.e. a cycle in the observed lock order graph.
// Detection happens before the blocking acquisition, so a proven defect is
// reported instead of hanging the process.
LQF_API std::size_t violation_count() noexcept;
LQF_API std::size_t reentrancy_count() noexcept;
LQF_API std::size_t inversion_count() noexcept;
LQF_API std::size_t tracked_lock_count() noexcept;
LQF_API std::size_t tracked_edge_count() noexcept;
LQF_API std::vector<LockViolation> recent_violations();
LQF_API std::string violation_report();
LQF_API void reset_tracking() noexcept;

namespace detail {
LQF_API void before_acquire(const void* id, const char* name, LockMode mode) noexcept;
LQF_API void after_release(const void* id) noexcept;
LQF_API std::size_t held_depth() noexcept;
}  // namespace detail

// A mutex whose acquisitions are audited. The audit is a no-op build option
// (LQF_LOCK_TRACKING=0) but the type stays identical so calling code never
// changes shape between configurations.
class LQF_API TrackedMutex {
 public:
  explicit TrackedMutex(const char* name) noexcept : name_(name) {}
  ~TrackedMutex() = default;

  TrackedMutex(const TrackedMutex&) = delete;
  TrackedMutex& operator=(const TrackedMutex&) = delete;
  TrackedMutex(TrackedMutex&&) = delete;
  TrackedMutex& operator=(TrackedMutex&&) = delete;

  void lock() {
    detail::before_acquire(this, name_, LockMode::Exclusive);
    mutex_.lock();
  }
  bool try_lock() {
    detail::before_acquire(this, name_, LockMode::Exclusive);
    if (mutex_.try_lock()) {
      return true;
    }
    detail::after_release(this);
    return false;
  }
  void unlock() {
    mutex_.unlock();
    detail::after_release(this);
  }
  void lock_shared() {
    detail::before_acquire(this, name_, LockMode::Shared);
    mutex_.lock_shared();
  }
  bool try_lock_shared() {
    detail::before_acquire(this, name_, LockMode::Shared);
    if (mutex_.try_lock_shared()) {
      return true;
    }
    detail::after_release(this);
    return false;
  }
  void unlock_shared() {
    mutex_.unlock_shared();
    detail::after_release(this);
  }

  [[nodiscard]] const char* name() const noexcept { return name_; }

 private:
  std::shared_mutex mutex_{};
  const char* name_{"unnamed"};
};

// Exclusive scope. Never callbacks out while holding one.
class LQF_API UniqueLock {
 public:
  explicit UniqueLock(TrackedMutex& mutex) : mutex_(&mutex) { mutex_->lock(); }
  ~UniqueLock() {
    if (mutex_ != nullptr) {
      mutex_->unlock();
    }
  }

  UniqueLock(const UniqueLock&) = delete;
  UniqueLock& operator=(const UniqueLock&) = delete;
  UniqueLock(UniqueLock&& other) noexcept : mutex_(other.mutex_) { other.mutex_ = nullptr; }
  UniqueLock& operator=(UniqueLock&& other) noexcept {
    if (this != &other) {
      if (mutex_ != nullptr) {
        mutex_->unlock();
      }
      mutex_ = other.mutex_;
      other.mutex_ = nullptr;
    }
    return *this;
  }

 private:
  TrackedMutex* mutex_{nullptr};
};

class LQF_API SharedLock {
 public:
  explicit SharedLock(TrackedMutex& mutex) : mutex_(&mutex) { mutex_->lock_shared(); }
  ~SharedLock() {
    if (mutex_ != nullptr) {
      mutex_->unlock_shared();
    }
  }

  SharedLock(const SharedLock&) = delete;
  SharedLock& operator=(const SharedLock&) = delete;
  SharedLock(SharedLock&& other) noexcept : mutex_(other.mutex_) { other.mutex_ = nullptr; }
  SharedLock& operator=(SharedLock&& other) noexcept {
    if (this != &other) {
      if (mutex_ != nullptr) {
        mutex_->unlock_shared();
      }
      mutex_ = other.mutex_;
      other.mutex_ = nullptr;
    }
    return *this;
  }

 private:
  TrackedMutex* mutex_{nullptr};
};

}  // namespace lqf::locks

#endif  // LQF_CORE_LOCK_TRACKER_HPP
