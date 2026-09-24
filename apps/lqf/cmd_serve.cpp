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

// lqf serve -- run a FabricServer on 127.0.0.1 until it is stopped.

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "cmd_common.hpp"
#include "lqf/core/text.hpp"
#include "lqf/runtime/config.hpp"
#include "lqf/transport/server.hpp"

namespace lqf::cli {
namespace {

CommandSpec serve_spec() {
  CommandSpec spec;
  spec.name = "serve";
  spec.summary = "run a FabricServer on 127.0.0.1 until it is stopped";
  spec.options = {
      {"--journal", true, "<path>", "journal file to persist and recover (required)"},
      {"--port", true, "<n>", "loopback port; 0 selects an ephemeral port (default 0)"},
      {"--shutdown-token", true, "<token>", "token that arms the Shutdown protocol message"},
      {"--clock", true, "<system|epoch>",
       "system uses the wall clock; epoch uses a deterministic clock at the Unix epoch and a "
       "fixed fabric epoch of 1"},
  };
  spec.required = {"--journal"};
  return spec;
}

// A deterministic clock for replayable runs: wall time starts at the Unix epoch
// and never advances on its own.
std::shared_ptr<Clock> make_clock(const std::string& mode, Status& status) {
  if (mode == "system") {
    return make_system_clock();
  }
  if (mode == "epoch") {
    return std::make_shared<ManualClock>(0, 0);
  }
  status = Status::error(StatusCode::Invalid,
                         "--clock accepts either system or epoch, received: " + mode);
  return nullptr;
}

}  // namespace

int run_serve(const std::vector<std::string>& argv) {
  const CommandSpec spec = serve_spec();
  if (requests_help(argv)) {
    print_help(std::cout, spec);
    return kExitOk;
  }
  const ParseResult parsed = parse_arguments(spec, argv);
  if (!parsed.ok) {
    return usage_error(spec, parsed.error);
  }

  u16 port = 0;
  if (const std::string* text = parsed.args.find("--port")) {
    Outcome<u16> value = parse_port_value(*text, true);
    if (!value.ok()) {
      return usage_error(spec, value.status().message());
    }
    port = value.value();
  }

  Status clock_status = Status::success();
  const std::string clock_mode = parsed.args.value_or("--clock", "system");
  std::shared_ptr<Clock> clock = make_clock(clock_mode, clock_status);
  if (clock == nullptr) {
    return usage_error(spec, clock_status.message());
  }

  FabricConfig config;
  config.journal.enabled = true;
  config.journal.path = *parsed.args.find("--journal");
  config.clock = std::move(clock);
  if (clock_mode == "epoch") {
    // A deterministic clock is only reproducible if the process incarnation is
    // fixed too; otherwise every report would carry a different epoch.
    config.forced_epoch = FabricEpoch(1);
  }

  Outcome<std::shared_ptr<Fabric>> fabric = Fabric::open(std::move(config));
  if (!fabric.ok()) {
    return report_failure(fabric.status());
  }

  ServerConfig server_config;
  server_config.bind_address = "127.0.0.1";
  server_config.port = port;
  server_config.shutdown_token = parsed.args.value_or("--shutdown-token", std::string());
  // An explanation reply carries the full deterministic report text, which is
  // longer than the 128 byte default string bound of the wire codec.
  server_config.codec.max_string_bytes = 4096;

  FabricServer server(fabric.value(), server_config);
  const Status started = server.start();
  if (!started.ok()) {
    return report_failure(started);
  }

  // The port is printed first and flushed so that a supervising process can
  // read it without parsing anything else.
  std::cout << "listening " << server.port() << "\n";
  std::cout.flush();
  std::cout << "journal " << parsed.args.value_or("--journal", std::string()) << "\n";
  std::cout << "clock " << clock_mode << "\n";
  std::cout << "shutdown-token "
            << (server_config.shutdown_token.empty() ? "disabled" : "armed") << "\n";
  std::cout << render_epoch_and_policy(fabric.value()->epoch(), fabric.value()->current_policy())
            << "\n";
  std::cout.flush();

  server.wait();
  const Status stopped = server.stop();
  if (!stopped.ok()) {
    return report_failure(stopped);
  }
  const Status closed = fabric.value()->close();
  if (!closed.ok()) {
    return report_failure(closed);
  }
  std::cout << "stopped " << server.port() << "\n";
  return kExitOk;
}

}  // namespace lqf::cli
