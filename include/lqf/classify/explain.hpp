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

#ifndef LQF_CLASSIFY_EXPLAIN_HPP
#define LQF_CLASSIFY_EXPLAIN_HPP

#include <string>

#include "lqf/classify/classifier.hpp"
#include "lqf/domain/classification.hpp"

namespace lqf {

// A derivation always names the policy generation and the evidence that
// produced it, and the rendering is byte identical for identical inputs.
LQF_API Explanation explain_link(const ClassifierContext& context, const QualityQuery& query);

LQF_API std::string render_policy_document(const PolicyDocument& document);
LQF_API std::string render_capability_view(const MetricCapabilityView& view);

}  // namespace lqf

#endif  // LQF_CLASSIFY_EXPLAIN_HPP
