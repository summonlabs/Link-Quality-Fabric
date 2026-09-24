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

#include "lqf/domain/metric.hpp"

#include <algorithm>

#include "lqf/core/assert.hpp"
#include "lqf/core/text.hpp"

namespace lqf {
namespace {

struct FamilyName {
  MetricFamily family;
  const char* name;
  const char* description;
};

constexpr FamilyName kFamilies[] = {
    {MetricFamily::Unspecified, "unspecified", "no family declared"},
    {MetricFamily::SignalPower, "signal-power", "received or transmitted signal level"},
    {MetricFamily::SignalRatio, "signal-ratio", "signal to noise ratio or margin"},
    {MetricFamily::ErrorCounter, "error-counter", "cumulative error events"},
    {MetricFamily::LossRatio, "loss-ratio", "loss expressed as a ratio"},
    {MetricFamily::Timing, "timing", "latency, jitter and skew"},
    {MetricFamily::Throughput, "throughput", "cumulative volume or a rate"},
    {MetricFamily::Environmental, "environmental", "temperature and similar"},
    {MetricFamily::Bias, "bias", "transducer bias current or voltage"},
    {MetricFamily::LinkStateHint, "link-state-hint", "opaque transport provided hint"},
};

bool name_is_valid(std::string_view name) {
  if (name.empty() || name.size() > kMaxMetricNameBytes) {
    return false;
  }
  if (name.front() == '.' || name.back() == '.') {
    return false;
  }
  bool previous_dot = false;
  for (const char character : name) {
    const bool alnum = (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
    if (character == '.') {
      if (previous_dot) {
        return false;
      }
      previous_dot = true;
      continue;
    }
    if (!alnum && character != '_' && character != '-') {
      return false;
    }
    previous_dot = false;
  }
  return true;
}

}  // namespace

const char* to_string(MetricFamily family) noexcept {
  for (const FamilyName& entry : kFamilies) {
    if (entry.family == family) {
      return entry.name;
    }
  }
  return "invalid";
}

std::string_view describe(MetricFamily family) noexcept {
  for (const FamilyName& entry : kFamilies) {
    if (entry.family == family) {
      return entry.description;
    }
  }
  return "unknown family";
}

Status parse_metric_family(std::string_view text_value, MetricFamily& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  for (const FamilyName& entry : kFamilies) {
    if (lowered == entry.name) {
      out = entry.family;
      return Status::success();
    }
  }
  return Status::error(StatusCode::Invalid, "unknown metric family: " + std::string(text_value));
}

const char* to_string(SampleSemantics semantics) noexcept {
  switch (semantics) {
    case SampleSemantics::Gauge: return "gauge";
    case SampleSemantics::Counter: return "counter";
  }
  return "invalid";
}

Status parse_sample_semantics(std::string_view text_value, SampleSemantics& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  if (lowered == "gauge") {
    out = SampleSemantics::Gauge;
    return Status::success();
  }
  if (lowered == "counter") {
    out = SampleSemantics::Counter;
    return Status::success();
  }
  return Status::error(StatusCode::Invalid, "unknown sample semantics: " + std::string(text_value));
}

const char* to_string(CounterWidth width) noexcept {
  switch (width) {
    case CounterWidth::Unspecified: return "unspecified";
    case CounterWidth::Bits32: return "u32";
    case CounterWidth::Bits64: return "u64";
  }
  return "invalid";
}

std::optional<u64> counter_modulus(CounterWidth width) noexcept {
  switch (width) {
    case CounterWidth::Bits32: return 1ULL << 32U;
    case CounterWidth::Bits64: return 0ULL;  // wraps at 2^64, which is not representable
    case CounterWidth::Unspecified: return std::nullopt;
  }
  return std::nullopt;
}

bool counter_value_in_range(u64 value, CounterWidth width) noexcept {
  switch (width) {
    case CounterWidth::Bits32: return value <= 0xFFFFFFFFULL;
    case CounterWidth::Bits64: return true;
    case CounterWidth::Unspecified: return true;
  }
  return true;
}

Status parse_counter_width(std::string_view text_value, CounterWidth& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  if (lowered == "unspecified" || lowered == "none") {
    out = CounterWidth::Unspecified;
  } else if (lowered == "u32" || lowered == "32" || lowered == "bits32") {
    out = CounterWidth::Bits32;
  } else if (lowered == "u64" || lowered == "64" || lowered == "bits64") {
    out = CounterWidth::Bits64;
  } else {
    return Status::error(StatusCode::Invalid, "unknown counter width: " + std::string(text_value));
  }
  return Status::success();
}

const char* to_string(MetricBasis basis) noexcept {
  switch (basis) {
    case MetricBasis::GaugeValue: return "gauge-value";
    case MetricBasis::CounterRatePerSecond: return "counter-rate-per-second";
    case MetricBasis::CounterDelta: return "counter-delta";
  }
  return "invalid";
}

Status parse_metric_basis(std::string_view text_value, MetricBasis& out) {
  const std::string lowered = text::to_lower_ascii(text::trim_ascii(text_value));
  if (lowered == "gauge-value" || lowered == "gauge") {
    out = MetricBasis::GaugeValue;
  } else if (lowered == "counter-rate-per-second" || lowered == "rate") {
    out = MetricBasis::CounterRatePerSecond;
  } else if (lowered == "counter-delta" || lowered == "delta") {
    out = MetricBasis::CounterDelta;
  } else {
    return Status::error(StatusCode::Invalid, "unknown metric basis: " + std::string(text_value));
  }
  return Status::success();
}

Status validate_metric_id(const MetricId& id) {
  if (id.family == MetricFamily::Unspecified) {
    return Status::error(StatusCode::Invalid, "metric family is unspecified");
  }
  if (id.family >= MetricFamily::Count) {
    return Status::error(StatusCode::Invalid, "metric family is out of range");
  }
  if (!name_is_valid(id.name)) {
    return Status::error(StatusCode::Invalid,
                         "metric name must be lower case dotted words of at most " +
                             std::to_string(kMaxMetricNameBytes) + " bytes: " + id.name);
  }
  return Status::success();
}

std::string render_metric_id(const MetricId& id) {
  std::string out = to_string(id.family);
  out.push_back(':');
  out.append(id.name);
  return out;
}

MetricCatalog::MetricCatalog(std::size_t max_descriptors)
    : max_descriptors_(max_descriptors == 0 ? 1 : max_descriptors) {}

Status MetricCatalog::register_descriptor(MetricDescriptor descriptor) {
  const Status id_status = validate_metric_id(descriptor.id);
  if (!id_status.ok()) {
    return id_status;
  }
  if (descriptor.unit == Unit::None) {
    return Status::error(StatusCode::Invalid,
                         "metric descriptor must declare units: " + render_metric_id(descriptor.id));
  }
  const bool counter = descriptor.semantics == SampleSemantics::Counter;
  if (counter && descriptor.unit != Unit::Count && descriptor.unit != Unit::Bytes) {
    return Status::error(StatusCode::Invalid,
                         "counter metric must be declared in count or bytes: " +
                             render_metric_id(descriptor.id));
  }
  if (!counter && (descriptor.unit == Unit::Count || descriptor.unit == Unit::Bytes)) {
    return Status::error(StatusCode::Invalid,
                         "gauge metric must not be declared in count or bytes: " +
                             render_metric_id(descriptor.id));
  }
  const MetricBasis expected = counter ? MetricBasis::CounterRatePerSecond : MetricBasis::GaugeValue;
  if (counter && descriptor.basis == MetricBasis::GaugeValue) {
    return Status::error(StatusCode::Invalid,
                         "counter metric basis must be a counter basis: " +
                             render_metric_id(descriptor.id));
  }
  if (!counter && descriptor.basis != MetricBasis::GaugeValue) {
    return Status::error(StatusCode::Invalid,
                         "gauge metric basis must be gauge-value: " + render_metric_id(descriptor.id));
  }
  (void)expected;
  for (const MetricDescriptor& existing : descriptors_) {
    if (existing.id == descriptor.id) {
      if (existing.unit == descriptor.unit && existing.semantics == descriptor.semantics) {
        return Status::error(StatusCode::Duplicate,
                             "metric descriptor already registered: " + render_metric_id(descriptor.id));
      }
      return Status::error(StatusCode::Conflict,
                           "metric descriptor conflicts with the registered one: " +
                               render_metric_id(descriptor.id));
    }
  }
  if (descriptors_.size() >= max_descriptors_) {
    return Status::error(StatusCode::LimitExceeded, "metric catalog capacity reached");
  }
  descriptor.description.resize(std::min<std::size_t>(descriptor.description.size(), 160));
  descriptors_.push_back(std::move(descriptor));
  return Status::success();
}

const MetricDescriptor* MetricCatalog::find(const MetricId& id) const {
  for (const MetricDescriptor& descriptor : descriptors_) {
    if (descriptor.id == id) {
      return &descriptor;
    }
  }
  return nullptr;
}

bool MetricCatalog::contains(const MetricId& id) const { return find(id) != nullptr; }

std::vector<MetricDescriptor> MetricCatalog::snapshot() const {
  std::vector<MetricDescriptor> copy = descriptors_;
  std::sort(copy.begin(), copy.end(), [](const MetricDescriptor& lhs, const MetricDescriptor& rhs) {
    return lhs.id < rhs.id;
  });
  return copy;
}

MetricCatalog MetricCatalog::with_builtin_descriptors(std::size_t max_descriptors) {
  MetricCatalog catalog(max_descriptors);
  const auto add = [&catalog](MetricFamily family, const char* name, Unit unit,
                              SampleSemantics semantics, MetricBasis basis, CounterWidth width,
                              bool lanes, const char* description) {
    MetricDescriptor descriptor;
    descriptor.id = MetricId(family, name);
    descriptor.unit = unit;
    descriptor.semantics = semantics;
    descriptor.basis = basis;
    descriptor.counter_width = width;
    descriptor.supports_lanes = lanes;
    descriptor.description = description;
    const Status registered = catalog.register_descriptor(std::move(descriptor));
    LQF_ASSERT_MSG(registered.ok(), "a builtin metric descriptor failed to register");
    (void)registered;
  };

  add(MetricFamily::SignalPower, "rx.level", Unit::DecibelMilliwatt, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "received signal level");
  add(MetricFamily::SignalPower, "tx.level", Unit::DecibelMilliwatt, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "transmitted signal level");
  add(MetricFamily::SignalRatio, "snr", Unit::Decibel, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "signal to noise ratio");
  add(MetricFamily::SignalRatio, "margin", Unit::Decibel, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "margin to the decision threshold");
  add(MetricFamily::SignalPower, "eye.height", Unit::MilliVolt, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "electrical eye height");
  add(MetricFamily::Timing, "eye.width", Unit::PicoSecond, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "electrical eye width");
  add(MetricFamily::ErrorCounter, "errors.symbol", Unit::Count, SampleSemantics::Counter,
      MetricBasis::CounterRatePerSecond, CounterWidth::Bits64, true, "cumulative symbol errors");
  add(MetricFamily::ErrorCounter, "errors.frame", Unit::Count, SampleSemantics::Counter,
      MetricBasis::CounterRatePerSecond, CounterWidth::Bits64, true, "cumulative frame errors");
  add(MetricFamily::ErrorCounter, "errors.corrected", Unit::Count, SampleSemantics::Counter,
      MetricBasis::CounterRatePerSecond, CounterWidth::Bits64, true, "cumulative corrected errors");
  add(MetricFamily::ErrorCounter, "errors.uncorrectable", Unit::Count, SampleSemantics::Counter,
      MetricBasis::CounterRatePerSecond, CounterWidth::Bits64, true,
      "cumulative uncorrectable errors");
  add(MetricFamily::LossRatio, "loss.ratio", Unit::PartsPerMillion, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "loss ratio reported by the source");
  add(MetricFamily::Timing, "latency.roundtrip", Unit::NanoSecond, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "round trip latency");
  add(MetricFamily::Timing, "jitter.packet-delay", Unit::NanoSecond, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, true, "packet delay variation");
  add(MetricFamily::Throughput, "octets", Unit::Bytes, SampleSemantics::Counter,
      MetricBasis::CounterRatePerSecond, CounterWidth::Bits64, true, "cumulative octets");
  add(MetricFamily::Environmental, "temperature", Unit::MilliDegreeCelsius, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, false, "module temperature");
  add(MetricFamily::Bias, "bias.current", Unit::MicroAmpere, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, false, "bias current");
  add(MetricFamily::LinkStateHint, "transport.hint", Unit::Ratio, SampleSemantics::Gauge,
      MetricBasis::GaugeValue, CounterWidth::Unspecified, false,
      "opaque transport hint, consumed as evidence only and never as authority");
  return catalog;
}

}  // namespace lqf
