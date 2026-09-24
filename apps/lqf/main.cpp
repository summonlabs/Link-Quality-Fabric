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

// The lqf command line runtime. It is a thin, honest client of the library:
// every printed quality state is one the library returned, and every failure
// path prints the lqf::Status text and exits non-zero.

#include <iostream>
#include <string>
#include <vector>

#include "cmd_common.hpp"
#include "lqf/version.hpp"

namespace {

constexpr const char* kVersion = "lqf " LQF_VERSION_STRING;

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  if (arguments.empty()) {
    lqf::cli::print_top_level_usage(std::cerr);
    return lqf::cli::kExitUsage;
  }
  const std::string command = arguments.front();
  if (command == "--version") {
    std::cout << kVersion << "\n";
    return lqf::cli::kExitOk;
  }
  if (command == "--help" || command == "-h" || command == "help") {
    lqf::cli::print_top_level_usage(std::cout);
    return lqf::cli::kExitOk;
  }

  const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
  if (command == "serve") {
    return lqf::cli::run_serve(rest);
  }
  if (command == "shutdown") {
    return lqf::cli::run_shutdown(rest);
  }
  if (command == "journal-verify") {
    return lqf::cli::run_journal_verify(rest);
  }
  if (command == "journal-dump") {
    return lqf::cli::run_journal_dump(rest);
  }
  if (command == "emit") {
    return lqf::cli::run_emit(rest);
  }
  if (command == "query") {
    return lqf::cli::run_query(rest);
  }
  if (command == "explain") {
    return lqf::cli::run_explain(rest);
  }
  if (command == "inspect") {
    return lqf::cli::run_inspect(rest);
  }
  if (command == "metrics") {
    return lqf::cli::run_metrics(rest);
  }
  if (command == "policy") {
    return lqf::cli::run_policy(rest);
  }

  std::cerr << "lqf: unknown command: " << command << "\n";
  lqf::cli::print_top_level_usage(std::cerr);
  return lqf::cli::kExitUsage;
}
