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

// Journal inspection. Both commands read through lqf::JournalReader, which is
// the only supported way to interpret the on-disk format: the CLI never guesses
// at a record layout.

#include <iostream>
#include <string>
#include <vector>

#include "cmd_common.hpp"
#include "lqf/core/text.hpp"
#include "lqf/persist/journal.hpp"

namespace lqf::cli {
namespace {

CommandSpec verify_spec() {
  CommandSpec spec;
  spec.name = "journal-verify";
  spec.summary = "open a journal read-only and print the recovery report";
  spec.options = {
      {"--journal", true, "<path>", "journal file to verify (required)"},
  };
  spec.required = {"--journal"};
  return spec;
}

CommandSpec dump_spec() {
  CommandSpec spec;
  spec.name = "journal-dump";
  spec.summary = "print one line per journal record: ordinal, kind, body size";
  spec.options = {
      {"--journal", true, "<path>", "journal file to read (required)"},
      {"--limit", true, "<n>", "stop after n records"},
  };
  spec.required = {"--journal"};
  return spec;
}

// A journal that cannot be trusted is a failure, not a warning: a torn tail, a
// degraded replay or an unreadable header all exit non-zero.
int verdict(const RecoveryReport& report) {
  if (report.opened && report.header_valid && !report.torn_tail && !report.degraded &&
      !report.truncated_file && report.status.ok()) {
    return kExitOk;
  }
  Status status = report.status;
  if (status.ok()) {
    status = Status::error(StatusCode::Corrupt,
                           report.detail.empty() ? std::string("the journal is corrupt or degraded")
                                                 : report.detail);
  }
  std::cerr << "lqf: error: " << status.to_text() << "\n";
  return kExitFailure;
}

}  // namespace

int run_journal_verify(const std::vector<std::string>& argv) {
  const CommandSpec spec = verify_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }
  const std::string path = *parsed.args.find("--journal");
  if (!file_exists(path)) {
    return report_failure(StatusCode::NotFound, "journal file does not exist: " + path);
  }

  JournalConfig config;
  JournalReader reader;
  Outcome<RecoveryReport> opened = reader.open(path, config);
  if (!opened.ok()) {
    std::cout << "journal " << path << "\n";
    std::cout << render_recovery_report(reader.report());
    std::cout.flush();
    return report_failure(opened.status());
  }

  Status failure = Status::success();
  JournalRecord record;
  for (;;) {
    Outcome<bool> more = reader.next(record);
    if (!more.ok()) {
      failure = more.status();
      break;
    }
    if (!more.value()) {
      break;
    }
  }

  const RecoveryReport report = reader.report();
  reader.close();
  std::cout << "journal " << path << "\n";
  std::cout << render_recovery_report(report);
  std::cout.flush();
  if (!failure.ok()) {
    return report_failure(failure);
  }
  return verdict(report);
}

int run_journal_dump(const std::vector<std::string>& argv) {
  const CommandSpec spec = dump_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }
  const std::string path = *parsed.args.find("--journal");

  u64 limit = 0;
  if (const std::string* text = parsed.args.find("--limit")) {
    Outcome<u64> value = parse_u64_value(*text, "limit");
    if (!value.ok()) {
      return usage_error(spec, value.status().message());
    }
    if (value.value() == 0) {
      return usage_error(spec, "--limit must be at least 1");
    }
    limit = value.value();
  }
  if (!file_exists(path)) {
    return report_failure(StatusCode::NotFound, "journal file does not exist: " + path);
  }

  JournalConfig config;
  JournalReader reader;
  Outcome<RecoveryReport> opened = reader.open(path, config);
  if (!opened.ok()) {
    return report_failure(opened.status());
  }

  u64 printed = 0;
  Status failure = Status::success();
  JournalRecord record;
  for (;;) {
    if (limit != 0 && printed >= limit) {
      break;
    }
    Outcome<bool> more = reader.next(record);
    if (!more.ok()) {
      failure = more.status();
      break;
    }
    if (!more.value()) {
      break;
    }
    std::cout << text::format_u64(record.ordinal.value()) << " " << to_string(record.kind) << " "
              << text::format_u64(static_cast<u64>(record.body.size())) << "\n";
    printed += 1;
  }
  std::cout.flush();

  const RecoveryReport report = reader.report();
  reader.close();
  if (!failure.ok()) {
    return report_failure(failure);
  }
  if (report.torn_tail || report.degraded || !report.header_valid) {
    return verdict(report);
  }
  return kExitOk;
}

}  // namespace lqf::cli
