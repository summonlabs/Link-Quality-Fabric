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

#include "lqf/core/lock_tracker.hpp"

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lqf::locks {
namespace {

constexpr std::size_t kMaxHeldPerThread = 64;
constexpr std::size_t kMaxEdges = 1024;
constexpr std::size_t kMaxRecentViolations = 16;

struct Edge {
  const void* from{nullptr};
  const void* to{nullptr};
  std::string from_name{};
  std::string to_name{};
};

struct Registry {
  std::mutex mutex{};
  std::vector<Edge> edges{};
  std::vector<std::pair<const void*, std::string>> names{};
  std::vector<LockViolation> recent{};
  std::size_t violations{0};
  std::size_t reentrancy{0};
  std::size_t inversion{0};
  std::size_t edges_dropped{0};
};

Registry& registry() {
  static Registry instance;
  return instance;
}

// Depth first search over the recorded lock order graph. A path from the lock
// being acquired back to a lock already held by this thread is exactly the
// cycle that turns two individually correct critical sections into a deadlock.
bool reaches(const std::vector<Edge>& edges, const void* from, const void* target, int depth) {
  if (depth > 32) {
    return false;
  }
  for (const Edge& edge : edges) {
    if (edge.from != from) {
      continue;
    }
    if (edge.to == target) {
      return true;
    }
    if (reaches(edges, edge.to, target, depth + 1)) {
      return true;
    }
  }
  return false;
}

const char* name_of(const std::vector<std::pair<const void*, std::string>>& names, const void* id) {
  for (const auto& entry : names) {
    if (entry.first == id) {
      return entry.second.c_str();
    }
  }
  return "unknown";
}

void remember_name(Registry& reg, const void* id, const char* name) {
  for (const auto& entry : reg.names) {
    if (entry.first == id) {
      return;
    }
  }
  if (reg.names.size() < kMaxEdges) {
    reg.names.emplace_back(id, name == nullptr ? "unnamed" : std::string(name));
  }
}

}  // namespace

namespace detail {

thread_local std::vector<const void*> tls_held;
thread_local std::vector<std::string> tls_held_names;

void before_acquire(const void* id, const char* name, LockMode /*mode*/) noexcept {
#if defined(LQF_LOCK_TRACKING) && LQF_LOCK_TRACKING
  try {
    const char* own_name = name == nullptr ? "unnamed" : name;
    Registry& reg = registry();
    bool reentrant = false;
    for (const void* held : tls_held) {
      if (held == id) {
        reentrant = true;
        break;
      }
    }
    if (reentrant) {
      std::lock_guard<std::mutex> guard(reg.mutex);
      reg.violations += 1;
      reg.reentrancy += 1;
      LockViolation violation;
      violation.kind = ViolationKind::Reentrancy;
      violation.acquiring = own_name;
      violation.held = own_name;
      if (reg.recent.size() >= kMaxRecentViolations) {
        reg.recent.erase(reg.recent.begin());
      }
      reg.recent.push_back(std::move(violation));
    } else if (!tls_held.empty()) {
      std::lock_guard<std::mutex> guard(reg.mutex);
      for (std::size_t index = 0; index < tls_held.size(); ++index) {
        const void* held = tls_held[index];
        if (reaches(reg.edges, id, held, 0)) {
          reg.violations += 1;
          reg.inversion += 1;
          LockViolation violation;
          violation.kind = ViolationKind::OrderInversion;
          violation.acquiring = own_name;
          violation.held = name_of(reg.names, held);
          if (reg.recent.size() >= kMaxRecentViolations) {
            reg.recent.erase(reg.recent.begin());
          }
          reg.recent.push_back(std::move(violation));
        }
        remember_name(reg, id, own_name);
        remember_name(reg, held, index < tls_held_names.size() ? tls_held_names[index].c_str()
                                                              : "unknown");
        bool present = false;
        for (const Edge& edge : reg.edges) {
          if (edge.from == held && edge.to == id) {
            present = true;
            break;
          }
        }
        if (!present) {
          if (reg.edges.size() < kMaxEdges) {
            Edge edge;
            edge.from = held;
            edge.to = id;
            edge.from_name = name_of(reg.names, held);
            edge.to_name = own_name;
            reg.edges.push_back(std::move(edge));
          } else {
            reg.edges_dropped += 1;
          }
        }
      }
    }
    if (tls_held.size() < kMaxHeldPerThread) {
      tls_held.push_back(id);
      tls_held_names.emplace_back(own_name);
    }
  } catch (...) {
    // The audit must never change behaviour or throw out of an acquisition.
  }
#else
  (void)id;
  (void)name;
#endif
}

void after_release(const void* id) noexcept {
#if defined(LQF_LOCK_TRACKING) && LQF_LOCK_TRACKING
  for (std::size_t index = tls_held.size(); index > 0; --index) {
    if (tls_held[index - 1] == id) {
      tls_held.erase(tls_held.begin() + static_cast<std::ptrdiff_t>(index - 1));
      if (tls_held_names.size() >= index) {
        tls_held_names.erase(tls_held_names.begin() + static_cast<std::ptrdiff_t>(index - 1));
      }
      return;
    }
  }
#else
  (void)id;
#endif
}

std::size_t held_depth() noexcept { return tls_held.size(); }

}  // namespace detail

std::size_t violation_count() noexcept { return registry().violations; }
std::size_t reentrancy_count() noexcept { return registry().reentrancy; }
std::size_t inversion_count() noexcept { return registry().inversion; }

std::size_t tracked_lock_count() noexcept {
  Registry& reg = registry();
  std::lock_guard<std::mutex> guard(reg.mutex);
  return reg.names.size();
}

std::size_t tracked_edge_count() noexcept {
  Registry& reg = registry();
  std::lock_guard<std::mutex> guard(reg.mutex);
  return reg.edges.size();
}

std::vector<LockViolation> recent_violations() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> guard(reg.mutex);
  return reg.recent;
}

std::string violation_report() {
  Registry& reg = registry();
  std::lock_guard<std::mutex> guard(reg.mutex);
  std::string out;
  out.append("lock-audit violations=");
  out.append(std::to_string(reg.violations));
  out.append(" reentrancy=");
  out.append(std::to_string(reg.reentrancy));
  out.append(" inversion=");
  out.append(std::to_string(reg.inversion));
  out.append(" tracked=");
  out.append(std::to_string(reg.names.size()));
  out.append(" edges=");
  out.append(std::to_string(reg.edges.size()));
  out.append(" edges-dropped=");
  out.append(std::to_string(reg.edges_dropped));
  for (const LockViolation& violation : reg.recent) {
    out.append("\n  ");
    out.append(violation.kind == ViolationKind::Reentrancy ? "reentrancy" : "order-inversion");
    out.append(" acquiring=");
    out.append(violation.acquiring);
    out.append(" held=");
    out.append(violation.held);
  }
  return out;
}

void reset_tracking() noexcept {
  Registry& reg = registry();
  std::lock_guard<std::mutex> guard(reg.mutex);
  reg.edges.clear();
  reg.names.clear();
  reg.recent.clear();
  reg.violations = 0;
  reg.reentrancy = 0;
  reg.inversion = 0;
  reg.edges_dropped = 0;
}

}  // namespace lqf::locks
