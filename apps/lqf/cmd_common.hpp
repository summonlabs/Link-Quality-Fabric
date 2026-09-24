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

#ifndef LQF_APPS_LQF_CMD_COMMON_HPP
#define LQF_APPS_LQF_CMD_COMMON_HPP

// Shared plumbing for the lqf command line runtime: argument parsing that
// refuses anything it does not understand, deterministic renderings, and the
// exit code policy of the tool.
//
//   exit 0 - the operation completed and its result was printed
//   exit 1 - a library operation failed; the lqf::Status text is printed
//   exit 2 - the command line itself was unknown or malformed
//
// Nothing in this tool ever prints a success line for a failed operation and
// nothing ever prints derived state that the library did not return.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "lqf/core/status.hpp"
#include "lqf/domain/classification.hpp"
#include "lqf/domain/identity.hpp"
#include "lqf/domain/metric.hpp"
#include "lqf/persist/journal.hpp"
#include "lqf/runtime/fabric.hpp"

namespace lqf::cli {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitUsage = 2;

// ---------------------------------------------------------------------------
// Command line description and parsing
// ---------------------------------------------------------------------------

struct OptionSpec {
  std::string name{};        // "--port"
  bool takes_value{false};   // "--port 7788" or "--port=7788"
  std::string value_name{};  // "<n>", only used for the usage line
  std::string help{};
};

struct CommandSpec {
  std::string name{};
  std::string summary{};
  std::vector<OptionSpec> options{};
  std::vector<std::string> required{};
};

struct Args {
  std::map<std::string, std::string> values{};
  std::set<std::string> flags{};

  [[nodiscard]] bool has(std::string_view name) const;
  [[nodiscard]] const std::string* find(std::string_view name) const;
  [[nodiscard]] std::string value_or(std::string_view name, std::string fallback) const;
};

struct ParseResult {
  bool ok{false};
  Args args{};
  std::string error{};
};

[[nodiscard]] ParseResult parse_arguments(const CommandSpec& spec,
                                          const std::vector<std::string>& argv);

// "lqf <command> --help" prints the command help and exits successfully.
[[nodiscard]] inline bool requests_help(const std::vector<std::string>& argv) {
  for (const std::string& token : argv) {
    if (token == "--help" || token == "-h") {
      return true;
    }
  }
  return false;
}

// The single line printed to stderr for exit code 2.
[[nodiscard]] std::string usage_line(const CommandSpec& spec);
void print_usage(std::ostream& out, const CommandSpec& spec);
void print_help(std::ostream& out, const CommandSpec& spec);
void print_top_level_usage(std::ostream& out);

[[nodiscard]] int usage_error(const CommandSpec& spec, const std::string& message);
[[nodiscard]] int report_failure(const Status& status);
[[nodiscard]] int report_failure(StatusCode code, const std::string& message);

// ---------------------------------------------------------------------------
// Typed argument parsing. Every one of these returns a Status whose text is
// printed verbatim before exiting with kExitUsage.
// ---------------------------------------------------------------------------

// allow_zero is true only for a listener, where 0 selects an ephemeral port.
[[nodiscard]] Outcome<u16> parse_port_value(const std::string& text, bool allow_zero);
[[nodiscard]] Outcome<u64> parse_u64_value(const std::string& text, const char* what);
[[nodiscard]] Outcome<LinkIdentity> parse_link_value(const std::string& text);
[[nodiscard]] Outcome<SourceIdentity> parse_source_value(const std::string& text);
[[nodiscard]] Outcome<MetricId> parse_metric_value(const std::string& text);
[[nodiscard]] Outcome<LaneDimension> parse_lane_value(const std::string& text);

// ---------------------------------------------------------------------------
// Deterministic renderings built only from values the library returned.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string render_recovery_report(const RecoveryReport& report);
[[nodiscard]] std::string render_epoch_and_policy(const FabricEpoch& epoch,
                                                  const PolicyStamp& policy);
[[nodiscard]] std::string render_report_json(const LinkQualityReport& report);
[[nodiscard]] std::string render_seconds(double seconds);

// ---------------------------------------------------------------------------
// Offline access. The journal is copied before it is replayed so that a running
// server never observes a second writer, and the copy is removed afterwards.
// ---------------------------------------------------------------------------

class OfflineJournalFabric {
 public:
  OfflineJournalFabric(std::shared_ptr<Fabric> fabric, std::filesystem::path copy);
  ~OfflineJournalFabric();
  OfflineJournalFabric(const OfflineJournalFabric&) = delete;
  OfflineJournalFabric& operator=(const OfflineJournalFabric&) = delete;

  [[nodiscard]] Fabric& fabric() const noexcept { return *fabric_; }

 private:
  std::shared_ptr<Fabric> fabric_{};
  std::filesystem::path copy_{};
};

[[nodiscard]] Outcome<std::unique_ptr<OfflineJournalFabric>> open_offline_journal(
    const std::string& journal_path);

// ---------------------------------------------------------------------------
// Commands. Each entry point receives the argument vector with the command
// name removed and returns one of the exit codes above.
// ---------------------------------------------------------------------------

[[nodiscard]] int run_serve(const std::vector<std::string>& argv);
[[nodiscard]] int run_shutdown(const std::vector<std::string>& argv);
[[nodiscard]] int run_journal_verify(const std::vector<std::string>& argv);
[[nodiscard]] int run_journal_dump(const std::vector<std::string>& argv);
[[nodiscard]] int run_emit(const std::vector<std::string>& argv);
[[nodiscard]] int run_query(const std::vector<std::string>& argv);
[[nodiscard]] int run_explain(const std::vector<std::string>& argv);
[[nodiscard]] int run_inspect(const std::vector<std::string>& argv);
[[nodiscard]] int run_metrics(const std::vector<std::string>& argv);
[[nodiscard]] int run_policy(const std::vector<std::string>& argv);

}  // namespace lqf::cli

#endif  // LQF_APPS_LQF_CMD_COMMON_HPP
