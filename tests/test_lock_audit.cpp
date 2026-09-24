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

// The lock audit is only meaningful if it can detect a real defect. These cases
// prove detection first and then assert that the runtime itself never trips it.

#include "harness.hpp"

#include <thread>
#include <vector>

#include "lqf/core/lock_tracker.hpp"
#include "lqf/transport/client.hpp"
#include "lqf/transport/server.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");

}  // namespace

LQF_TEST(lock_audit, reentrancy_is_detected) {
  locks::reset_tracking();
  locks::TrackedMutex mutex("probe-reentrant");
  mutex.lock();
  // try_lock from the owning thread never succeeds for a shared mutex, so this
  // observes the audit without deadlocking the suite.
  LQF_CHECK(!mutex.try_lock());
  mutex.unlock();
  LQF_CHECK(locks::reentrancy_count() >= 1);
  LQF_CHECK(locks::violation_count() >= 1);
  const std::vector<locks::LockViolation> violations = locks::recent_violations();
  LQF_CHECK(!violations.empty());
  LQF_CHECK(violations.back().kind == locks::ViolationKind::Reentrancy);
  locks::reset_tracking();
  LQF_CHECK_EQ(locks::violation_count(), std::size_t{0});
}

LQF_TEST(lock_audit, order_inversion_is_detected) {
  locks::reset_tracking();
  locks::TrackedMutex first("probe-first");
  locks::TrackedMutex second("probe-second");
  {
    locks::UniqueLock outer(first);
    locks::UniqueLock inner(second);
  }
  LQF_CHECK_EQ(locks::inversion_count(), std::size_t{0});
  // The reverse order completes a cycle in the recorded lock order graph and is
  // reported instead of being allowed to deadlock later.
  {
    locks::UniqueLock outer(second);
    locks::UniqueLock inner(first);
  }
  LQF_CHECK(locks::inversion_count() >= 1);
  LQF_CHECK(locks::violation_count() >= 1);
  locks::reset_tracking();
  LQF_CHECK_EQ(locks::violation_count(), std::size_t{0});
}

LQF_TEST(lock_audit, runtime_workload_never_trips_the_audit) {
  locks::reset_tracking();
  TempDir directory("lock-audit");
  const std::string path = directory.file("state.lqf");

  FabricConfig config;
  config.clock = std::make_shared<ManualClock>();
  config.forced_epoch = FabricEpoch(0x10CCULL);
  config.journal.enabled = true;
  config.journal.path = path;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  LQF_CHECK(fabric.ok());

  ServerConfig server_config;
  server_config.port = 0;
  server_config.shutdown_token = "audit-token";
  FabricServer server(fabric.value(), server_config);
  LQF_CHECK_STATUS_OK(server.start());

  const LinkIdentity link(LinkId(std::string("link-audit")), LinkGeneration(1));
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < 3; ++index) {
    threads.emplace_back([&, index]() {
      const SourceIdentity source(SourceId(std::string("audit-src") + text::format_u64(index)),
                                  SourceIncarnation(1));
      if (!fabric.value()->declare_capability(make_capability(link, source, {kRxLevel})).ok()) {
        return;
      }
      ClientConfig client_config;
      client_config.port = server.port();
      Outcome<FabricClient> client = FabricClient::connect(client_config);
      if (!client.ok()) {
        return;
      }
      if (!client.value().handshake().ok()) {
        return;
      }
      for (u64 round = 0; round < 20; ++round) {
        (void)client.value().ingest({make_gauge(
            link, source, kRxLevel, -3.0, round + 1,
            1'700'000'000'000'000'000LL + static_cast<i64>(round) * 1'000'000'000LL,
            Unit::DecibelMilliwatt)});
        QualityQuery query;
        query.link = link;
        (void)client.value().query(query);
        (void)fabric.value()->query(query);
        (void)fabric.value()->inspect(InspectionFilter{});
      }
      client.value().close();
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  LQF_CHECK_STATUS_OK(fabric.value()->flush());
  LQF_CHECK_STATUS_OK(server.stop());
  LQF_CHECK_STATUS_OK(fabric.value()->close());

  // Zero violations across concurrent library and transport use is the claim
  // this repository makes; the two cases above prove the detector can fail.
  const std::string report = locks::violation_report();
  LQF_CHECK_MSG(locks::violation_count() == 0, report);
  LQF_CHECK_EQ(locks::reentrancy_count(), std::size_t{0});
  LQF_CHECK_EQ(locks::inversion_count(), std::size_t{0});
}
