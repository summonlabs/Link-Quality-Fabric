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

#ifndef LQF_TRANSPORT_CLIENT_HPP
#define LQF_TRANSPORT_CLIENT_HPP

#include <string>
#include <vector>

#include "lqf/runtime/fabric.hpp"
#include "lqf/transport/frame.hpp"
#include "lqf/transport/messages.hpp"
#include "lqf/transport/socket.hpp"

namespace lqf {

struct ClientConfig {
  u16 port{0};
  std::string address{"127.0.0.1"};
  FrameLimits frames{};
  CodecLimits codec{};
  std::string client_name{"lqf-client"};
  u64 client_epoch{0};
};

// One request at a time over one connection. Not thread safe by design: two
// threads sharing a client would interleave a request and a reply, which is a
// protocol error rather than a race this class can hide.
class LQF_API FabricClient {
 public:
  FabricClient() = default;
  ~FabricClient();
  FabricClient(const FabricClient&) = delete;
  FabricClient& operator=(const FabricClient&) = delete;
  FabricClient(FabricClient&&) noexcept = default;
  FabricClient& operator=(FabricClient&&) noexcept = default;

  static Outcome<FabricClient> connect(const ClientConfig& config);

  Status handshake();
  Outcome<CapabilityAck> declare_capability(const CapabilityDeclaration& declaration);
  Outcome<PolicyStamp> publish_policy(const PolicyDocument& document);
  Outcome<BatchOutcome> ingest(const std::vector<Observation>& observations);
  Outcome<LinkQualityReport> query(const QualityQuery& request);
  Outcome<WindowResult> window(const WindowQuery& request);
  Outcome<Explanation> explain(const QualityQuery& request);
  Outcome<InspectionReport> inspect(const InspectionFilter& filter);
  Outcome<std::vector<MetricCapabilityView>> capabilities(const LinkIdentity& link);
  Outcome<PolicyGenerationRecord> policy_document(PolicyGeneration generation);
  Outcome<FabricStats> stats();
  Outcome<FlushAck> flush();
  Outcome<CompactAck> compact();
  Outcome<ShutdownAck> shutdown(const std::string& token);

  void close();
  [[nodiscard]] bool connected() const noexcept { return connected_; }
  [[nodiscard]] FabricEpoch server_epoch() const noexcept { return server_epoch_; }

 private:
  Outcome<Frame> request(MessageType type, const std::string& body);
  Status send_frame(const Frame& frame);
  Outcome<Frame> receive_frame();

  Socket socket_{};
  ClientConfig config_{};
  FabricEpoch server_epoch_{};
  bool connected_{false};
};

}  // namespace lqf

#endif  // LQF_TRANSPORT_CLIENT_HPP
