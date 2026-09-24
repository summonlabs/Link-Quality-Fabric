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

#ifndef LQF_TESTS_HARNESS_HPP
#define LQF_TESTS_HARNESS_HPP

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lqf/core/checked.hpp"
#include "lqf/core/status.hpp"
#include "lqf/core/text.hpp"
#include "lqf/runtime/fabric.hpp"

namespace lqf::test {

using TestFunction = void (*)();

struct TestCase {
  std::string suite{};
  std::string name{};
  TestFunction function{nullptr};
};

// Registration is static: each suite file declares its cases with LQF_TEST.
void register_test(const char* suite, const char* name, TestFunction function);
std::vector<TestCase>& registry();

struct TestRegistration {
  TestRegistration(const char* suite, const char* name, TestFunction function) {
    register_test(suite, name, function);
  }
};

[[noreturn]] void fail(const char* file, int line, const std::string& message);

// Accepts either a Status or an Outcome and yields the Status, so the check
// macro can be used on every fallible call without a special case.
[[nodiscard]] inline Status as_status(const Status& status) { return status; }

template <class T>
[[nodiscard]] inline Status as_status(const Outcome<T>& outcome) {
  return outcome.status();
}

// A deterministic generator. Every property test prints the seed it used and
// the exact reproduction command on failure.
class Rng {
 public:
  explicit Rng(u64 seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

  u64 next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    u64 value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

  u64 below(u64 bound) { return bound == 0 ? 0 : next() % bound; }
  u64 range(u64 low, u64 high) { return low + below(high - low + 1); }
  double unit() { return static_cast<double>(next() >> 11U) * (1.0 / 9007199254740992.0); }
  bool chance(double probability) { return unit() < probability; }
  u64 seed() const { return seed_; }

 private:
  u64 state_{0};
  u64 seed_{0};
};

// A temporary directory that removes itself. Every test that touches the file
// system uses one, so no validation residue is left behind.
class TempDir {
 public:
  explicit TempDir(const std::string& label);
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;

 private:
  std::filesystem::path path_{};
};

[[nodiscard]] std::string executable_path();

// Wall clock nanoseconds. Tests that talk to a node running on the real system
// clock must stamp observations with a current time: a synchronized source whose
// evidence is older than the configured maximum observation age is refused.
[[nodiscard]] i64 wall_now_nanos();

// A child process running this same test binary. Used for the independent
// process proofs: threads are not processes.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept;
  ChildProcess& operator=(ChildProcess&& other) noexcept;

  static Outcome<ChildProcess> spawn(const std::vector<std::string>& arguments);

  [[nodiscard]] bool running();
  // Forceful termination, used as test input at a lifecycle boundary, never to
  // turn a hang into a pass.
  void terminate();
  [[nodiscard]] int wait();
  [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

 private:
  void* handle_{nullptr};
  u64 pid_{0};
  bool exited_{false};
  int exit_code_{0};
};

// Bounded readiness poll. Returns false when the condition never became true;
// a false result FAILS the test. Nothing in this suite passes because a wait
// expired.
bool wait_for_file(const std::string& path, int attempts = 4000);
bool wait_for_file_content(const std::string& path, std::string& content, int attempts = 4000);
void sleep_millis(int millis);

struct NodeOptions {
  std::string journal{};
  std::string port_file{};
  std::string shutdown_token{};
  bool synthetic_only{true};
};

// Runs this binary as a standalone node: opens a fabric with a journal, starts
// the transport, publishes the bound port to port_file, and waits for a
// shutdown request. Returns the process exit code.
int run_node(const NodeOptions& options);

struct TestOptions {
  std::string suite{};
  std::string filter{};
  u64 seed{0x5EED1234ULL};
  u64 iterations{0};
  bool list{false};
  bool verbose{false};
};

// Property tests read these so a failing run can be reproduced exactly with
// --seed and --iterations. Zero means "use the suite default".
u64 seed_override();
void set_seed_override(u64 value);
u64 iterations_override();
void set_iterations_override(u64 value);

int run_all(const TestOptions& options);

// Shared fixtures: a fabric with deterministic time and an in-memory catalog of
// declared capabilities for one link and one source.
struct Fixture {
  std::shared_ptr<ManualClock> clock{};
  std::shared_ptr<Fabric> fabric{};
  LinkIdentity link{};
  SourceIdentity source{};

  static Outcome<std::shared_ptr<Fixture>> create(const std::string& link_id = "link-a",
                                                  const std::string& source_id = "src-a",
                                                  bool with_journal = false,
                                                  const std::string& journal_path = std::string());
};

[[nodiscard]] CapabilityDeclaration make_capability(const LinkIdentity& link,
                                                   const SourceIdentity& source,
                                                   const std::vector<MetricId>& metrics,
                                                   std::vector<MetricId> unsupported = {});

[[nodiscard]] Observation make_gauge(const LinkIdentity& link, const SourceIdentity& source,
                                     const MetricId& metric, double value, u64 sequence,
                                     i64 observed_at_nanos, Unit unit,
                                     std::optional<LaneDimension> lane = std::nullopt,
                                     AuthorityRank authority = AuthorityRank(1),
                                     EvidenceClass evidence_class = EvidenceClass::Real);

[[nodiscard]] Observation make_counter(const LinkIdentity& link, const SourceIdentity& source,
                                       const MetricId& metric, u64 value, u64 sequence,
                                       i64 observed_at_nanos, Unit unit,
                                       CounterWidth width = CounterWidth::Bits64,
                                       std::optional<LaneDimension> lane = std::nullopt,
                                       AuthorityRank authority = AuthorityRank(1),
                                       bool reset_declared = false);

[[nodiscard]] const MetricDescriptor& descriptor_for(const MetricId& metric);

}  // namespace lqf::test

#define LQF_TEST(suite_name, case_name)                                                  \
  static void lqf_test_##suite_name##_##case_name();                                     \
  static const ::lqf::test::TestRegistration lqf_registration_##suite_name##_##case_name( \
      #suite_name, #case_name, &lqf_test_##suite_name##_##case_name);                     \
  static void lqf_test_##suite_name##_##case_name()

#define LQF_CHECK(condition)                                                       \
  do {                                                                             \
    if (!(condition)) {                                                            \
      ::lqf::test::fail(__FILE__, __LINE__, "check failed: " #condition);          \
    }                                                                              \
  } while (false)

#define LQF_CHECK_MSG(condition, message)                                          \
  do {                                                                             \
    if (!(condition)) {                                                            \
      ::lqf::test::fail(__FILE__, __LINE__, std::string("check failed: " #condition) + \
                                                  " -- " + (message));             \
    }                                                                              \
  } while (false)

#define LQF_CHECK_EQ(lhs, rhs)                                                          \
  do {                                                                                  \
    const auto& lqf_lhs = (lhs);                                                        \
    const auto& lqf_rhs = (rhs);                                                        \
    if (!(lqf_lhs == lqf_rhs)) {                                                        \
      ::lqf::test::fail(__FILE__, __LINE__,                                             \
                        std::string("expected equality: " #lhs " == " #rhs));           \
    }                                                                                   \
  } while (false)

#define LQF_CHECK_STATUS_OK(expression)                                                 \
  do {                                                                                  \
    const ::lqf::Status lqf_status = ::lqf::test::as_status(expression);                \
    if (!lqf_status.ok()) {                                                             \
      ::lqf::test::fail(__FILE__, __LINE__,                                             \
                        std::string("expected success: " #expression " -> ") +          \
                            lqf_status.to_text());                                      \
    }                                                                                   \
  } while (false)

#endif  // LQF_TESTS_HARNESS_HPP
