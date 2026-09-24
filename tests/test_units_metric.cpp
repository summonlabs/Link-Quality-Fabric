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

#include <cmath>
#include <limits>

using namespace lqf;
using namespace lqf::test;

LQF_TEST(units_metric, unit_classes_and_compatibility) {
  LQF_CHECK(unit_class(Unit::DecibelMilliwatt) == UnitClass::Power);
  LQF_CHECK(unit_class(Unit::MilliVolt) == UnitClass::Power);
  LQF_CHECK(unit_class(Unit::Decibel) == UnitClass::Ratio);
  LQF_CHECK(unit_class(Unit::PartsPerMillion) == UnitClass::Ratio);
  LQF_CHECK(unit_class(Unit::NanoSecond) == UnitClass::Time);
  LQF_CHECK(unit_class(Unit::Count) == UnitClass::Count);
  LQF_CHECK(unit_class(Unit::Bytes) == UnitClass::Count);
  LQF_CHECK(unit_class(Unit::BitsPerSecond) == UnitClass::Rate);
  LQF_CHECK(unit_class(Unit::MicroAmpere) == UnitClass::Current);

  LQF_CHECK(units_compatible(Unit::DecibelMilliwatt, Unit::MilliVolt));
  LQF_CHECK(!units_compatible(Unit::DecibelMilliwatt, Unit::NanoSecond));
  // An undeclared unit is never compatible with anything: a bare number is not
  // evidence.
  LQF_CHECK(!units_compatible(Unit::None, Unit::None));
  LQF_CHECK(!units_compatible(Unit::None, Unit::Count));
}

LQF_TEST(units_metric, unit_parse_round_trip) {
  const Unit units[] = {Unit::DecibelMilliwatt, Unit::Decibel,       Unit::MilliVolt,
                        Unit::Count,            Unit::PartsPerMillion, Unit::NanoSecond,
                        Unit::Bytes,            Unit::MicroAmpere,   Unit::MilliDegreeCelsius};
  for (const Unit unit : units) {
    Unit parsed = Unit::None;
    LQF_CHECK_STATUS_OK(parse_unit(to_string(unit), parsed));
    LQF_CHECK(parsed == unit);
    LQF_CHECK(!describe(unit).empty());
  }
  Unit parsed = Unit::None;
  LQF_CHECK(!parse_unit("furlongs", parsed).ok());
}

LQF_TEST(units_metric, metric_id_validation) {
  LQF_CHECK_STATUS_OK(validate_metric_id(MetricId(MetricFamily::SignalPower, "rx.level")));
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::Unspecified, "rx.level")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, "")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, "Rx.Level")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, "rx..level")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, ".rx")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, "rx.")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, "rx level")).ok());
  LQF_CHECK(!validate_metric_id(MetricId(MetricFamily::SignalPower, std::string(200, 'a'))).ok());
  LQF_CHECK_EQ(render_metric_id(MetricId(MetricFamily::ErrorCounter, "errors.frame")),
               std::string("error-counter:errors.frame"));
}

LQF_TEST(units_metric, catalog_registration_rules) {
  MetricCatalog catalog(2);
  MetricDescriptor gauge;
  gauge.id = MetricId(MetricFamily::SignalPower, "rx.level");
  gauge.unit = Unit::DecibelMilliwatt;
  gauge.semantics = SampleSemantics::Gauge;
  gauge.basis = MetricBasis::GaugeValue;
  LQF_CHECK_STATUS_OK(catalog.register_descriptor(gauge));

  // The same identity with different units is a conflict, not a silent
  // replacement.
  MetricDescriptor conflicting = gauge;
  conflicting.unit = Unit::Decibel;
  const Status conflict = catalog.register_descriptor(conflicting);
  LQF_CHECK(conflict.code() == StatusCode::Conflict);

  // Identical re-registration is a duplicate.
  const Status duplicate = catalog.register_descriptor(gauge);
  LQF_CHECK(duplicate.code() == StatusCode::Duplicate);

  // A counter must be declared in count or bytes.
  MetricDescriptor bad_counter;
  bad_counter.id = MetricId(MetricFamily::ErrorCounter, "errors.frame");
  bad_counter.unit = Unit::Decibel;
  bad_counter.semantics = SampleSemantics::Counter;
  bad_counter.basis = MetricBasis::CounterRatePerSecond;
  const Status invalid = catalog.register_descriptor(bad_counter);
  LQF_CHECK(invalid.code() == StatusCode::Invalid);

  // A gauge must not be declared in count.
  MetricDescriptor bad_gauge;
  bad_gauge.id = MetricId(MetricFamily::Throughput, "octets");
  bad_gauge.unit = Unit::Count;
  bad_gauge.semantics = SampleSemantics::Gauge;
  bad_gauge.basis = MetricBasis::GaugeValue;
  LQF_CHECK(!catalog.register_descriptor(bad_gauge).ok());

  // Unit-less metrics are refused outright.
  MetricDescriptor unitless;
  unitless.id = MetricId(MetricFamily::Timing, "latency.roundtrip");
  unitless.unit = Unit::None;
  unitless.semantics = SampleSemantics::Gauge;
  unitless.basis = MetricBasis::GaugeValue;
  LQF_CHECK(!catalog.register_descriptor(unitless).ok());

  // Capacity is enforced.
  MetricDescriptor extra;
  extra.id = MetricId(MetricFamily::Timing, "jitter.packet-delay");
  // Capacity is two, so this fills the catalog.
  extra.unit = Unit::NanoSecond;
  extra.semantics = SampleSemantics::Gauge;
  extra.basis = MetricBasis::GaugeValue;
  LQF_CHECK_STATUS_OK(catalog.register_descriptor(extra));
  LQF_CHECK_EQ(catalog.size(), std::size_t{2});
  MetricDescriptor overflow;
  overflow.id = MetricId(MetricFamily::Environmental, "temperature");
  overflow.unit = Unit::MilliDegreeCelsius;
  overflow.semantics = SampleSemantics::Gauge;
  overflow.basis = MetricBasis::GaugeValue;
  LQF_CHECK(catalog.register_descriptor(overflow).code() == StatusCode::LimitExceeded);
}

LQF_TEST(units_metric, builtin_descriptors_are_vendor_neutral) {
  const MetricCatalog catalog = MetricCatalog::with_builtin_descriptors();
  LQF_CHECK(catalog.size() >= 15);
  const MetricDescriptor* rx = catalog.find(MetricId(MetricFamily::SignalPower, "rx.level"));
  LQF_CHECK(rx != nullptr);
  LQF_CHECK(rx->unit == Unit::DecibelMilliwatt);
  LQF_CHECK(rx->semantics == SampleSemantics::Gauge);
  const MetricDescriptor* errors =
      catalog.find(MetricId(MetricFamily::ErrorCounter, "errors.uncorrectable"));
  LQF_CHECK(errors != nullptr);
  LQF_CHECK(errors->semantics == SampleSemantics::Counter);
  LQF_CHECK(errors->basis == MetricBasis::CounterRatePerSecond);
  LQF_CHECK(errors->counter_width == CounterWidth::Bits64);
  const MetricDescriptor* hint =
      catalog.find(MetricId(MetricFamily::LinkStateHint, "transport.hint"));
  LQF_CHECK(hint != nullptr);
  LQF_CHECK(!hint->supports_lanes);
  // Nothing in the catalog names a vendor.
  for (const MetricDescriptor& descriptor : catalog.snapshot()) {
    const std::string name = descriptor.id.name;
    LQF_CHECK(name.find("vendor") == std::string::npos);
    LQF_CHECK(name.find("cisco") == std::string::npos);
    LQF_CHECK(name.find("nvidia") == std::string::npos);
  }
}

LQF_TEST(units_metric, counter_width_semantics) {
  LQF_CHECK(!counter_modulus(CounterWidth::Unspecified).has_value());
  LQF_CHECK_EQ(counter_modulus(CounterWidth::Bits32).value(), u64{1} << 32U);
  LQF_CHECK(counter_modulus(CounterWidth::Bits64).has_value());
  LQF_CHECK_EQ(counter_modulus(CounterWidth::Bits64).value(), u64{0});
  LQF_CHECK(counter_value_in_range(0xFFFFFFFFULL, CounterWidth::Bits32));
  LQF_CHECK(!counter_value_in_range(0x100000000ULL, CounterWidth::Bits32));
  LQF_CHECK(counter_value_in_range(std::numeric_limits<u64>::max(), CounterWidth::Bits64));
  // An unspecified width cannot disprove anything.
  LQF_CHECK(counter_value_in_range(std::numeric_limits<u64>::max(), CounterWidth::Unspecified));
}
