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

#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include "lqf/core/lock_tracker.hpp"
#include "lqf/core/text.hpp"
#include "lqf/transport/server.hpp"

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lqf::test {
namespace {

struct TestFailure {
  std::string message{};
};

std::atomic<std::size_t> g_temp_counter{0};
u64 g_seed_override{0};
u64 g_iterations_override{0};

u64 current_process_id() {
#if defined(_WIN32)
  return static_cast<u64>(_getpid());
#else
  return static_cast<u64>(getpid());
#endif
}

}  // namespace

u64 seed_override() { return g_seed_override; }
void set_seed_override(u64 value) { g_seed_override = value; }
u64 iterations_override() { return g_iterations_override; }
void set_iterations_override(u64 value) { g_iterations_override = value; }

void register_test(const char* suite, const char* name, TestFunction function) {
  TestCase test;
  test.suite = suite;
  test.name = name;
  test.function = function;
  registry().push_back(std::move(test));
}

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

void fail(const char* file, int line, const std::string& message) {
  std::ostringstream stream;
  stream << file << ":" << line << ": " << message;
  throw TestFailure{stream.str()};
}

TempDir::TempDir(const std::string& label) {
  const std::size_t counter = g_temp_counter.fetch_add(1);
  path_ = std::filesystem::temp_directory_path() /
          ("lqf-test-" + label + "-" + text::format_u64(current_process_id()) + "-" +
           text::format_u64(counter));
  std::error_code error;
  std::filesystem::remove_all(path_, error);
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const { return (path_ / name).string(); }

std::string executable_path() {
#if defined(_WIN32)
  char buffer[MAX_PATH * 4] = {0};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  if (length == 0) {
    return {};
  }
  return std::string(buffer, length);
#else
  char buffer[4096] = {0};
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) {
    return {};
  }
  return std::string(buffer, static_cast<std::size_t>(length));
#endif
}

i64 wall_now_nanos() {
  return static_cast<i64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void sleep_millis(int millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

ChildProcess::~ChildProcess() {
  if (handle_ != nullptr && !exited_) {
    terminate();
    (void)wait();
  }
#if defined(_WIN32)
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#endif
}

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : handle_(other.handle_), pid_(other.pid_), exited_(other.exited_), exit_code_(other.exit_code_) {
  other.handle_ = nullptr;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
  if (this != &other) {
    if (handle_ != nullptr && !exited_) {
      terminate();
      (void)wait();
    }
#if defined(_WIN32)
    if (handle_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(handle_));
    }
#endif
    handle_ = other.handle_;
    pid_ = other.pid_;
    exited_ = other.exited_;
    exit_code_ = other.exit_code_;
    other.handle_ = nullptr;
  }
  return *this;
}

Outcome<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& arguments) {
  const std::string program = executable_path();
  if (program.empty()) {
    return Status::error(StatusCode::Internal, "the test binary path could not be resolved");
  }
  std::string command = "\"" + program + "\"";
  for (const std::string& argument : arguments) {
    command.append(" \"");
    command.append(argument);
    command.push_back('\"');
  }
#if defined(_WIN32)
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                     &startup, &information) == 0) {
    return Status::error(StatusCode::Io,
                         "child process could not be created, error " +
                             text::format_u64(static_cast<u64>(GetLastError())));
  }
  CloseHandle(information.hThread);
  ChildProcess child;
  child.handle_ = information.hProcess;
  child.pid_ = information.dwProcessId;
  return child;
#else
  const pid_t pid = fork();
  if (pid < 0) {
    return Status::error(StatusCode::Io, "child process could not be forked");
  }
  if (pid == 0) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    execv(program.c_str(), argv.data());
    _exit(127);
  }
  ChildProcess child;
  child.pid_ = static_cast<u64>(pid);
  child.handle_ = reinterpret_cast<void*>(1);
  return child;
#endif
}

bool ChildProcess::running() {
  if (handle_ == nullptr || exited_) {
    return false;
  }
#if defined(_WIN32)
  const DWORD result = WaitForSingleObject(static_cast<HANDLE>(handle_), 0);
  return result == WAIT_TIMEOUT;
#else
  int status = 0;
  const pid_t result = waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  exited_ = true;
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  return false;
#endif
}

void ChildProcess::terminate() {
  if (handle_ == nullptr || exited_) {
    return;
  }
#if defined(_WIN32)
  (void)TerminateProcess(static_cast<HANDLE>(handle_), 9);
#else
  (void)kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
}

int ChildProcess::wait() {
  if (handle_ == nullptr) {
    return exit_code_;
  }
  if (exited_) {
    return exit_code_;
  }
#if defined(_WIN32)
  (void)WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
  DWORD code = 0;
  if (GetExitCodeProcess(static_cast<HANDLE>(handle_), &code) != 0) {
    exit_code_ = static_cast<int>(code);
  }
#else
  int status = 0;
  (void)waitpid(static_cast<pid_t>(pid_), &status, 0);
  exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  exited_ = true;
  return exit_code_;
}

bool wait_for_file(const std::string& path, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::error_code error;
    if (std::filesystem::exists(path, error) && !error) {
      return true;
    }
    sleep_millis(5);
  }
  return false;
}

bool wait_for_file_content(const std::string& path, std::string& content, int attempts) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    std::ifstream stream(path, std::ios::binary);
    if (stream) {
      std::ostringstream buffer;
      buffer << stream.rdbuf();
      content = buffer.str();
      if (!content.empty()) {
        return true;
      }
    }
    sleep_millis(5);
  }
  return false;
}

int run_node(const NodeOptions& options) {
  FabricConfig config;
  config.journal.enabled = true;
  config.journal.path = options.journal;
  config.journal.fsync_on_flush = true;
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  if (!fabric.ok()) {
    std::fprintf(stderr, "node: fabric open failed: %s\n", fabric.status().to_text().c_str());
    return 3;
  }
  ServerConfig server_config;
  server_config.port = 0;
  server_config.shutdown_token = options.shutdown_token;
  FabricServer server(fabric.value(), server_config);
  const Status started = server.start();
  if (!started.ok()) {
    std::fprintf(stderr, "node: server start failed: %s\n", started.to_text().c_str());
    return 4;
  }
  const std::string ready = "READY " + text::format_u64(server.port()) + "\n";
  {
    std::ofstream stream(options.port_file, std::ios::binary | std::ios::trunc);
    stream << ready;
    stream.flush();
  }
  server.wait();
  const Status stopped = server.stop();
  const Status closed = fabric.value()->close();
  if (!stopped.ok()) {
    std::fprintf(stderr, "node: server stop failed: %s\n", stopped.to_text().c_str());
    return 5;
  }
  if (!closed.ok()) {
    std::fprintf(stderr, "node: fabric close failed: %s\n", closed.to_text().c_str());
    return 6;
  }
  return 0;
}

Outcome<std::shared_ptr<Fixture>> Fixture::create(const std::string& link_id,
                                                  const std::string& source_id, bool with_journal,
                                                  const std::string& journal_path) {
  auto fixture = std::make_shared<Fixture>();
  fixture->clock = std::make_shared<ManualClock>();
  FabricConfig config;
  config.clock = fixture->clock;
  config.has_initial_policy = false;
  config.forced_epoch = FabricEpoch(0x1234ULL);
  if (with_journal) {
    config.journal.enabled = true;
    config.journal.path = journal_path;
  }
  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(config);
  if (!fabric.ok()) {
    return fabric.status();
  }
  fixture->fabric = fabric.value();
  fixture->link = LinkIdentity(LinkId(std::string(link_id)), LinkGeneration(1));
  fixture->source = SourceIdentity(SourceId(std::string(source_id)), SourceIncarnation(1));
  return fixture;
}

const MetricDescriptor& descriptor_for(const MetricId& metric) {
  static const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  const MetricDescriptor* descriptor = catalog.find(metric);
  if (descriptor == nullptr) {
    fail(__FILE__, __LINE__, "metric is missing from the builtin catalog");
  }
  return *descriptor;
}

CapabilityDeclaration make_capability(const LinkIdentity& link, const SourceIdentity& source,
                                      const std::vector<MetricId>& metrics,
                                      std::vector<MetricId> unsupported) {
  CapabilityDeclaration declaration;
  declaration.source = source;
  declaration.link_scope = link;
  declaration.revision = CapabilityRevision(1);
  declaration.transport = TransportKind::Ethernet;
  declaration.evidence_class = EvidenceClass::Synthetic;
  for (const MetricId& metric : metrics) {
    const MetricDescriptor& descriptor = descriptor_for(metric);
    MetricCapability capability;
    capability.metric = metric;
    capability.unit = descriptor.unit;
    capability.semantics = descriptor.semantics;
    capability.counter_width = descriptor.counter_width;
    capability.lanes_declared = false;
    declaration.metrics.push_back(capability);
  }
  declaration.unsupported_metrics = std::move(unsupported);
  return declaration;
}

Observation make_gauge(const LinkIdentity& link, const SourceIdentity& source,
                       const MetricId& metric, double value, u64 sequence, i64 observed_at_nanos,
                       Unit unit, std::optional<LaneDimension> lane, AuthorityRank authority,
                       EvidenceClass evidence_class) {
  Observation observation;
  observation.link = link;
  observation.source = source;
  observation.sequence = SequenceNumber(sequence);
  observation.metric = metric;
  if (lane.has_value()) {
    observation.lane = *lane;
  }
  observation.unit = unit;
  GaugeReading reading;
  reading.value = value;
  reading.validity_nanos = 0;
  observation.reading = reading;
  observation.observed_at = Timestamp{observed_at_nanos, true};
  observation.authority = authority;
  observation.provenance.transport = TransportKind::Ethernet;
  observation.provenance.evidence_class = evidence_class;
  observation.provenance.origin = "test-harness";
  observation.provenance.producer = "lqf-tests";
  return observation;
}

Observation make_counter(const LinkIdentity& link, const SourceIdentity& source,
                         const MetricId& metric, u64 value, u64 sequence, i64 observed_at_nanos,
                         Unit unit, CounterWidth width, std::optional<LaneDimension> lane,
                         AuthorityRank authority, bool reset_declared) {
  Observation observation;
  observation.link = link;
  observation.source = source;
  observation.sequence = SequenceNumber(sequence);
  observation.metric = metric;
  if (lane.has_value()) {
    observation.lane = *lane;
  }
  observation.unit = unit;
  CounterReading reading;
  reading.value = value;
  reading.width = width;
  reading.reset_declared = reset_declared;
  observation.reading = reading;
  observation.observed_at = Timestamp{observed_at_nanos, true};
  observation.authority = authority;
  observation.provenance.transport = TransportKind::Ethernet;
  observation.provenance.evidence_class = EvidenceClass::Synthetic;
  observation.provenance.origin = "test-harness";
  observation.provenance.producer = "lqf-tests";
  return observation;
}

int run_all(const TestOptions& options) {
  g_seed_override = options.seed;
  g_iterations_override = options.iterations;
  std::vector<TestCase> selected;
  for (const TestCase& test : registry()) {
    if (!options.suite.empty() && test.suite != options.suite) {
      continue;
    }
    if (!options.filter.empty() && test.name.find(options.filter) == std::string::npos) {
      continue;
    }
    selected.push_back(test);
  }
  std::sort(selected.begin(), selected.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });

  if (options.list) {
    for (const TestCase& test : selected) {
      std::printf("%s.%s\n", test.suite.c_str(), test.name.c_str());
    }
    return 0;
  }
  if (selected.empty()) {
    std::fprintf(stderr, "no test matched suite '%s' filter '%s'\n", options.suite.c_str(),
                 options.filter.c_str());
    return 1;
  }

  std::size_t passed = 0;
  std::vector<std::string> failures;
  for (const TestCase& test : selected) {
    const std::string label = test.suite + "." + test.name;
    try {
      test.function();
      passed += 1;
      if (options.verbose) {
        std::printf("PASS %s\n", label.c_str());
      }
    } catch (const TestFailure& failure) {
      failures.push_back(label + ": " + failure.message);
      std::printf("FAIL %s\n  %s\n", label.c_str(), failure.message.c_str());
    } catch (const std::exception& error) {
      failures.push_back(label + ": unexpected exception: " + error.what());
      std::printf("FAIL %s\n  unexpected exception: %s\n", label.c_str(), error.what());
    } catch (...) {
      failures.push_back(label + ": unknown exception");
      std::printf("FAIL %s\n  unknown exception\n", label.c_str());
    }
    std::fflush(stdout);
  }

  const std::size_t violations = locks::violation_count();
  if (violations != 0) {
    failures.push_back("lock audit reported violations: " + locks::violation_report());
    std::printf("FAIL lock-audit\n%s\n", locks::violation_report().c_str());
  }

  std::printf("\n%d of %d test cases passed in suite '%s'%s%s\n", static_cast<int>(passed),
              static_cast<int>(selected.size()),
              options.suite.empty() ? "(all)" : options.suite.c_str(),
              options.filter.empty() ? "" : " filtered", failures.empty() ? "" : " WITH FAILURES");
  for (const std::string& failure : failures) {
    std::printf("  failure: %s\n", failure.c_str());
  }
  return failures.empty() ? 0 : 1;
}

}  // namespace lqf::test
