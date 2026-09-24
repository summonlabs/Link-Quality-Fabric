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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"

namespace {

std::string argument_value(const std::vector<std::string>& arguments, const std::string& name,
                           const std::string& fallback = std::string()) {
  for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
    if (arguments[index] == name) {
      return arguments[index + 1];
    }
  }
  return fallback;
}

bool has_argument(const std::vector<std::string>& arguments, const std::string& name) {
  for (const std::string& argument : arguments) {
    if (argument == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  arguments.reserve(static_cast<std::size_t>(argc));
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }

  // Child node mode: this same binary runs as an independent process for the
  // multiprocess and restart proofs.
  if (has_argument(arguments, "--node-journal")) {
    lqf::test::NodeOptions options;
    options.journal = argument_value(arguments, "--node-journal");
    options.port_file = argument_value(arguments, "--node-port-file");
    options.shutdown_token = argument_value(arguments, "--node-token", "lqf-test-token");
    if (options.journal.empty() || options.port_file.empty()) {
      std::fprintf(stderr, "node mode requires --node-journal and --node-port-file\n");
      return 2;
    }
    return lqf::test::run_node(options);
  }

  lqf::test::TestOptions options;
  options.suite = argument_value(arguments, "--suite");
  options.filter = argument_value(arguments, "--filter");
  options.list = has_argument(arguments, "--list");
  options.verbose = has_argument(arguments, "--verbose");
  const std::string seed = argument_value(arguments, "--seed");
  if (!seed.empty()) {
    lqf::u64 value = 0;
    const lqf::Status status = lqf::text::parse_u64(seed, value);
    if (!status.ok()) {
      std::fprintf(stderr, "malformed --seed\n");
      return 2;
    }
    options.seed = value;
  }
  const std::string iterations = argument_value(arguments, "--iterations");
  if (!iterations.empty()) {
    lqf::u64 value = 0;
    const lqf::Status status = lqf::text::parse_u64(iterations, value);
    if (!status.ok()) {
      std::fprintf(stderr, "malformed --iterations\n");
      return 2;
    }
    options.iterations = value;
  }
  return lqf::test::run_all(options);
}
