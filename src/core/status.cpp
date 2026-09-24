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

#include "lqf/core/status.hpp"

namespace lqf {

const char* to_string(StatusCode code) noexcept {
  switch (code) {
    case StatusCode::Ok: return "ok";
    case StatusCode::Invalid: return "invalid";
    case StatusCode::NotFound: return "not-found";
    case StatusCode::Unsupported: return "unsupported";
    case StatusCode::Conflict: return "conflict";
    case StatusCode::Stale: return "stale";
    case StatusCode::Fenced: return "fenced";
    case StatusCode::Rejected: return "rejected";
    case StatusCode::Duplicate: return "duplicate";
    case StatusCode::IdMismatch: return "id-mismatch";
    case StatusCode::Overflow: return "overflow";
    case StatusCode::LimitExceeded: return "limit-exceeded";
    case StatusCode::Corrupt: return "corrupt";
    case StatusCode::Unavailable: return "unavailable";
    case StatusCode::Busy: return "busy";
    case StatusCode::Cancelled: return "cancelled";
    case StatusCode::Refused: return "refused";
    case StatusCode::Protocol: return "protocol";
    case StatusCode::Io: return "io";
    case StatusCode::Internal: return "internal";
  }
  return "unknown";
}

Status Status::error(StatusCode code, std::string message) {
  Status status;
  status.code_ = code;
  status.message_ = std::move(message);
  return status;
}

std::string Status::to_text() const {
  if (ok()) {
    return "ok";
  }
  std::string text = to_string(code_);
  if (!message_.empty()) {
    text.append(": ");
    text.append(message_);
  }
  return text;
}

}  // namespace lqf
