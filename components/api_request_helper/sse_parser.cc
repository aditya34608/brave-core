/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "brave/components/api_request_helper/sse_parser.h"

#include <utility>

#include "base/check.h"
#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/trace_event/trace_event.h"
#include "brave/components/api_request_helper/utils.h"

namespace api_request_helper {

namespace {
constexpr std::string_view kDataField = "data";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
}  // namespace

SSEParser::SSEParser(scoped_refptr<base::SequencedTaskRunner> task_runner,
                     base::RepeatingCallback<void(ValueOrError)> on_event)
    : task_runner_(std::move(task_runner)), on_event_(std::move(on_event)) {}

SSEParser::~SSEParser() = default;

void SSEParser::Process(std::string_view chunk) {
  if (chunk.empty()) {
    return;
  }

  std::string stream_start;
  if (at_stream_start_) {
    // A BOM may be split across chunks, so hold back a prefix of one.
    stream_start = base::StrCat({line_buffer_, chunk});
    if (stream_start.size() < kUtf8Bom.size() &&
        kUtf8Bom.starts_with(stream_start)) {
      line_buffer_ = std::move(stream_start);
      return;
    }
    line_buffer_.clear();
    at_stream_start_ = false;
    chunk = stream_start;
    if (chunk.starts_with(kUtf8Bom)) {
      chunk.remove_prefix(kUtf8Bom.size());
    }
  }

  if (skip_leading_lf_ && chunk.starts_with('\n')) {
    chunk.remove_prefix(1);
  }
  skip_leading_lf_ = false;

  size_t pos;
  while ((pos = chunk.find_first_of("\r\n")) != std::string_view::npos) {
    const std::string_view line = chunk.substr(0, pos);
    if (line_buffer_.empty()) {
      ProcessLine(line);
    } else {
      line_buffer_.append(line);
      ProcessLine(line_buffer_);
      line_buffer_.clear();
    }

    size_t line_ending_size = 1;
    if (chunk[pos] == '\r') {
      if (pos + 1 == chunk.size()) {
        skip_leading_lf_ = true;
      } else if (chunk[pos + 1] == '\n') {
        line_ending_size = 2;
      }
    }
    chunk.remove_prefix(pos + line_ending_size);
  }
  line_buffer_.append(chunk);
}

void SSEParser::Flush() {
  line_buffer_.clear();
  skip_leading_lf_ = false;
  at_stream_start_ = true;
  DispatchEvent();
}

void SSEParser::Clear() {
  line_buffer_.clear();
  data_buffer_.clear();
  skip_leading_lf_ = false;
  at_stream_start_ = true;
}

bool SSEParser::IsDecoding() const {
  return decoding_count_ > 0;
}

void SSEParser::ProcessLine(std::string_view line) {
  if (line.empty()) {
    DispatchEvent();
    return;
  }

  // A line without ':' is a field name with an empty value. Comment lines
  // start with ':', so have an empty field name and are ignored.
  const size_t colon = line.find(':');
  if (line.substr(0, colon) != kDataField) {
    return;
  }
  std::string_view value;
  if (colon != std::string_view::npos) {
    value = line.substr(colon + 1);
    if (value.starts_with(' ')) {
      value.remove_prefix(1);
    }
  }
  data_buffer_.append(value);
  data_buffer_.push_back('\n');
}

void SSEParser::DispatchEvent() {
  if (data_buffer_.empty()) {
    return;
  }
  std::string data = std::move(data_buffer_);
  data_buffer_.clear();
  // Drop the "\n" that follows the last value.
  data.pop_back();

  if (!base::TrimWhitespaceASCII(data, base::TRIM_LEADING).starts_with('{')) {
    return;
  }

  decoding_count_++;
  ParseJsonInWorkerTaskRunner(
      std::move(data), task_runner_.get(),
      base::BindOnce(&SSEParser::OnJsonParsed, weak_ptr_factory_.GetWeakPtr()));
}

void SSEParser::OnJsonParsed(ValueOrError result) {
  TRACE_EVENT0("brave", "APIRequestHelper_ParseSSECallback");
  CHECK_GT(decoding_count_, 0u);
  decoding_count_--;
  DCHECK(on_event_);
  // May delete `this`.
  on_event_.Run(std::move(result));
}

}  // namespace api_request_helper
