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

#include "lqf/classify/explain.hpp"

using namespace lqf;
using namespace lqf::test;

namespace {

const MetricId kRxLevel(MetricFamily::SignalPower, "rx.level");

}  // namespace

LQF_TEST(explain_derivation, explanation_names_policy_rules_and_evidence) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -3.0, 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::DecibelMilliwatt)));

  QualityQuery query;
  query.link = self.link;
  Outcome<Explanation> explanation = self.fabric->explain(query);
  LQF_CHECK(explanation.ok());
  const std::string& text = explanation.value().text;
  // The derivation names the link, the generation, the policy generation, the
  // content hash, the rule that produced the state and the evidence itself.
  LQF_CHECK(text.find(render_link_identity(self.link)) != std::string::npos);
  LQF_CHECK(text.find("gen1") != std::string::npos);
  LQF_CHECK(text.find(explanation.value().report.policy.content_hash) != std::string::npos);
  LQF_CHECK(text.find("rx-level-healthy") != std::string::npos);
  LQF_CHECK(text.find("band-matched") != std::string::npos);
  LQF_CHECK(text.find("test-harness") != std::string::npos);
  LQF_CHECK(text.find("healthy") != std::string::npos);
}

LQF_TEST(explain_derivation, rendering_is_byte_identical_across_instances) {
  const auto build = [](std::string& rendered) -> bool {
    auto fixture = Fixture::create();
    if (!fixture.ok()) {
      return false;
    }
    const auto& self = *fixture.value();
    if (!self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})).ok()) {
      return false;
    }
    if (!self.fabric->ingest(make_gauge(self.link, self.source, kRxLevel, -12.0, 1, 1'700'000'000'000'000'000LL,
                                        Unit::DecibelMilliwatt))
             .ok()) {
      return false;
    }
    QualityQuery query;
    query.link = self.link;
    Outcome<Explanation> explanation = self.fabric->explain(query);
    if (!explanation.ok()) {
      return false;
    }
    rendered = explanation.value().text;
    return true;
  };

  std::string first;
  std::string second;
  LQF_CHECK(build(first));
  LQF_CHECK(build(second));
  LQF_CHECK_EQ(first, second);
  LQF_CHECK(first.find("degraded") != std::string::npos);
}

LQF_TEST(explain_derivation, renderings_cover_policy_and_capability) {
  const PolicyDocument document = default_policy_document();
  const std::string policy_text = render_policy_document(document);
  LQF_CHECK(policy_text.find("default") != std::string::npos);
  LQF_CHECK(policy_text.find("rx-level-healthy") != std::string::npos);
  LQF_CHECK(policy_text.find("severity") != std::string::npos);

  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {kRxLevel})));
  Outcome<std::vector<MetricCapabilityView>> views = self.fabric->capabilities(self.link);
  LQF_CHECK(views.ok());
  LQF_CHECK_EQ(views.value().size(), std::size_t{1});
  const std::string view_text = render_capability_view(views.value()[0]);
  LQF_CHECK(view_text.find("rx.level") != std::string::npos);
  LQF_CHECK(view_text.find("declared-by=1") != std::string::npos);
}

LQF_TEST(explain_derivation, unclassified_metric_says_why) {
  auto fixture = Fixture::create();
  LQF_CHECK(fixture.ok());
  const auto& self = *fixture.value();
  const MetricId temperature(MetricFamily::Environmental, "temperature");
  LQF_CHECK_STATUS_OK(
      self.fabric->declare_capability(make_capability(self.link, self.source, {temperature})));
  LQF_CHECK_STATUS_OK(self.fabric->ingest(make_gauge(self.link, self.source, temperature, 41000.0, 1,
                                                     self.clock->now().wall_nanos,
                                                     Unit::MilliDegreeCelsius)));
  QualityQuery query;
  query.link = self.link;
  Outcome<Explanation> explanation = self.fabric->explain(query);
  LQF_CHECK(explanation.ok());
  // There is no default rule for temperature, and the runtime refuses to invent
  // one: the state is unknown with an explicit reason.
  LQF_CHECK(explanation.value().report.overall == QualityState::Unknown);
  LQF_CHECK(explanation.value().text.find("no-rule-for-metric") != std::string::npos);
  LQF_CHECK(explanation.value().text.find("healthy") == std::string::npos);
}
