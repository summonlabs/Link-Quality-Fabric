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

// Decoder hardening: every frame in this suite is attacker controlled input.

#include "harness.hpp"

#include <limits>
#include <string>
#include <vector>

#include "lqf/core/hash.hpp"
#include "lqf/transport/frame.hpp"
#include "lqf/transport/messages.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

std::string frame_bytes(u32 length, u32 crc, u8 type, const std::string& body) {
  std::string out(kFrameHeaderBytes, '\0');
  for (std::size_t index = 0; index < 4; ++index) {
    out[index] = static_cast<char>((length >> (index * 8U)) & 0xFFU);
    out[4 + index] = static_cast<char>((crc >> (index * 8U)) & 0xFFU);
  }
  out.push_back(static_cast<char>(type));
  out.append(body);
  return out;
}

template <class T>
std::string encode_value(const T& value) {
  CodecLimits limits;
  Writer writer(limits);
  Encoder encoder(writer, limits);
  encoder.field(value);
  return writer.ok() ? writer.buffer() : std::string();
}

}  // namespace

LQF_TEST(wire_codec, round_trip_is_exact_for_every_message) {
  const LinkIdentity link(LinkId(std::string("link-w")), LinkGeneration(3));
  const SourceIdentity source(SourceId(std::string("src-w")), SourceIncarnation(2));
  const MetricId metric(MetricFamily::SignalPower, "rx.level");

  HelloRequest hello;
  hello.protocol_version = 1;
  hello.client_name = "lqf-tests";
  hello.client_epoch = 42;
  {
    const std::string body = encode_value(hello);
    Reader reader(body, CodecLimits{});
    Decoder decoder(reader, CodecLimits{});
    HelloRequest decoded;
    decoder.field(decoded);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(reader.empty());
    LQF_CHECK_EQ(decoded.client_name, hello.client_name);
    LQF_CHECK_EQ(decoded.client_epoch, hello.client_epoch);
  }

  Observation observation = make_gauge(link, source, metric, -3.25, 9, 1'700'000'000'000'000'000LL,
                                       Unit::DecibelMilliwatt, LaneDimension(LaneId(3)),
                                       AuthorityRank(4));
  observation.interval_nanos = 1'000'000LL;
  {
    const std::string body = encode_value(observation);
    Reader reader(body, CodecLimits{});
    Decoder decoder(reader, CodecLimits{});
    Observation decoded;
    decoder.field(decoded);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(reader.empty());
    LQF_CHECK(decoded == observation);
    // Encoding the decoded value reproduces the identical bytes.
    LQF_CHECK_EQ(encode_value(decoded), body);
  }

  LinkQualityReport report;
  report.link = link;
  report.overall = QualityState::Conflicting;
  report.confidence = ConfidenceLevel::Low;
  add_reason(report.reasons, ReasonCode::EqualAuthorityDisagreement);
  MetricAssessment assessment;
  assessment.metric = metric;
  assessment.state = QualityState::Degraded;
  assessment.value = -12.5;
  EvidenceRef reference;
  reference.source = source;
  reference.sequence = SequenceNumber(5);
  reference.value = -12.5;
  reference.rendered = "-12.5 dBm";
  assessment.evidence.push_back(reference);
  report.metrics.push_back(assessment);
  {
    const std::string body = encode_value(report);
    Reader reader(body, CodecLimits{});
    Decoder decoder(reader, CodecLimits{});
    LinkQualityReport decoded;
    decoder.field(decoded);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(reader.empty());
    LQF_CHECK_EQ(render_report(decoded), render_report(report));
    LQF_CHECK_EQ(encode_value(decoded), body);
  }

  PolicyDocument policy = default_policy_document();
  {
    const std::string body = encode_value(policy);
    Reader reader(body, CodecLimits{});
    Decoder decoder(reader, CodecLimits{});
    PolicyDocument decoded;
    decoder.field(decoded);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(reader.empty());
    LQF_CHECK_EQ(canonical_policy_text(decoded), canonical_policy_text(policy));
  }

  CapabilityDeclaration declaration = make_capability(link, source, {metric});
  {
    const std::string body = encode_value(declaration);
    Reader reader(body, CodecLimits{});
    Decoder decoder(reader, CodecLimits{});
    CapabilityDeclaration decoded;
    decoder.field(decoded);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(reader.empty());
    LQF_CHECK_EQ(encode_value(decoded), body);
  }
}

LQF_TEST(wire_codec, malformed_frames_are_refused) {
  FrameLimits limits;
  Frame frame;
  const std::string payload = std::string(1, static_cast<char>(MessageType::Stats));

  // A frame that declares a length above the limit is refused before the body
  // is read.
  {
    const std::string bytes = frame_bytes(0x00FFFFFFU, 0, static_cast<u8>(MessageType::Stats),
                                          std::string());
    u32 length = 0;
    u32 crc = 0;
    const Status status = decode_frame_header(
        reinterpret_cast<const unsigned char*>(bytes.data()), limits, length, crc);
    LQF_CHECK(!status.ok());
    LQF_CHECK(status.code() == StatusCode::Protocol);
  }
  // A zero length frame is refused.
  {
    const std::string bytes = frame_bytes(0, 0, static_cast<u8>(MessageType::Stats), std::string());
    u32 length = 0;
    u32 crc = 0;
    LQF_CHECK(!decode_frame_header(reinterpret_cast<const unsigned char*>(bytes.data()), limits,
                                   length, crc)
                   .ok());
  }
  // A checksum mismatch is refused.
  LQF_CHECK(!decode_frame_payload(payload, crc32c(payload) ^ 1U, frame).ok());
  // An unknown message type is refused.
  {
    const std::string unknown = std::string(1, static_cast<char>(250));
    LQF_CHECK(!decode_frame_payload(unknown, crc32c(unknown), frame).ok());
  }
  // A well formed frame decodes.
  LQF_CHECK_STATUS_OK(decode_frame_payload(payload, crc32c(payload), frame));
  LQF_CHECK(frame.type == MessageType::Stats);
  LQF_CHECK(frame.body.empty());

  // Encoding an unknown message type is refused rather than emitted.
  Frame invalid;
  invalid.type = MessageType::Invalid;
  std::string out;
  LQF_CHECK(!encode_frame(invalid, out, limits).ok());

  // Oversized payloads are refused at encode time with checked arithmetic.
  Frame oversized;
  oversized.type = MessageType::Ingest;
  oversized.body.assign(static_cast<std::size_t>(limits.max_frame_bytes) + 1U, 'x');
  LQF_CHECK(encode_frame(oversized, out, limits).code() == StatusCode::LimitExceeded);
}

LQF_TEST(wire_codec, decoder_bounds_every_field) {
  CodecLimits limits;
  limits.max_string_bytes = 16;
  limits.max_items = 4;
  limits.max_total_bytes = 1024;

  const auto decode_observation = [&](const std::string& body, Reader& reader, Observation& out) {
    Decoder decoder(reader, limits);
    decoder.field(out);
    (void)body;
    return reader.ok();
  };

  // A truncated message fails instead of reading past the end.
  {
    const std::string body = encode_value(make_gauge(
        LinkIdentity(LinkId(std::string("l")), LinkGeneration(1)),
        SourceIdentity(SourceId(std::string("s")), SourceIncarnation(1)),
        MetricId(MetricFamily::SignalPower, "rx.level"), -3.0, 1, 1, Unit::DecibelMilliwatt));
    for (std::size_t cut = 1; cut < body.size(); cut += 3) {
      const std::string truncated = body.substr(0, body.size() - cut);
      Reader reader(truncated, limits);
      Observation observation;
      LQF_CHECK(!decode_observation(truncated, reader, observation));
      LQF_CHECK(!reader.status().ok());
    }
    // Trailing bytes are a protocol error at the call sites that require an
    // exact message. The buffer must outlive the reader: Reader holds a view,
    // which is what keeps decoding allocation free.
    const std::string with_trailing = body + "junk";
    Reader reader(with_trailing, limits);
    Observation observation;
    Decoder decoder(reader, limits);
    decoder.field(observation);
    LQF_CHECK(decoder.ok());
    LQF_CHECK(!reader.empty());
  }

  // A vector count larger than the remaining input is refused before any
  // allocation happens.
  {
    std::string body;
    Writer writer(limits);
    writer.put_u32(1000000U);
    body = writer.buffer();
    Reader reader(body, limits);
    std::vector<Observation> observations;
    Decoder decoder(reader, limits);
    decoder.field(observations);
    LQF_CHECK(!reader.ok());
    LQF_CHECK(!observations.empty() ? observations.size() <= limits.max_items : true);
  }

  // A string whose declared length exceeds the bound is refused.
  {
    Writer writer(limits);
    writer.put_u32(1000U);
    writer.put_bytes(std::string(1000, 'a'));
    Reader reader(writer.buffer(), limits);
    std::string out;
    LQF_CHECK(!reader.get_text(out, limits.max_string_bytes));
    LQF_CHECK(!reader.ok());
  }

  // An enum value that is out of range is refused.
  {
    Writer writer(limits);
    writer.put_u32(9999U);
    Reader reader(writer.buffer(), limits);
    QualityState state = QualityState::Unknown;
    Decoder decoder(reader, limits);
    decoder.field(state);
    LQF_CHECK(!reader.ok());
  }

  // A boolean that is neither zero nor one is refused.
  {
    Writer writer(limits);
    writer.put_u8(2);
    Reader reader(writer.buffer(), limits);
    bool value = false;
    Decoder decoder(reader, limits);
    decoder.field(value);
    LQF_CHECK(!reader.ok());
  }

  // A variant index that does not exist is refused.
  {
    Writer writer(limits);
    writer.put_u8(9);
    Reader reader(writer.buffer(), limits);
    Reading reading{GaugeReading{}};
    Decoder decoder(reader, limits);
    decoder.field(reading);
    LQF_CHECK(!reader.ok());
  }

  // A NaN double survives the codec bit for bit, so the boundary check is the
  // only place that decides it is not evidence.
  {
    const double nan_value = std::numeric_limits<double>::quiet_NaN();
    Writer writer(limits);
    writer.put_f64(nan_value);
    Reader reader(writer.buffer(), limits);
    double decoded = 0.0;
    LQF_CHECK(reader.get_f64(decoded));
    LQF_CHECK(std::isnan(decoded));
  }
}

LQF_TEST(wire_codec, error_replies_round_trip) {
  ErrorReply reply;
  reply.code = StatusCode::Conflict;
  reply.message = "equal authority disagreement";
  std::string body;
  LQF_CHECK_STATUS_OK(encode_error_reply(reply, body));
  ErrorReply decoded;
  LQF_CHECK_STATUS_OK(decode_error_reply(body, decoded));
  LQF_CHECK(decoded.code == StatusCode::Conflict);
  LQF_CHECK_EQ(decoded.message, reply.message);

  // A refusal without a message still round trips.
  ErrorReply bare;
  bare.code = StatusCode::Refused;
  LQF_CHECK_STATUS_OK(encode_error_reply(bare, body));
  LQF_CHECK_STATUS_OK(decode_error_reply(body, decoded));
  LQF_CHECK(decoded.code == StatusCode::Refused);
  LQF_CHECK(decoded.message.empty());

  // An out of range code is refused.
  std::string forged;
  forged.push_back(static_cast<char>(250));
  LQF_CHECK(!decode_error_reply(forged, decoded).ok());
}
