/* Copyright (c) 2026 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef BRAVE_COMPONENTS_API_REQUEST_HELPER_SSE_PARSER_H_
#define BRAVE_COMPONENTS_API_REQUEST_HELPER_SSE_PARSER_H_

#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace api_request_helper {

using ValueOrError = base::expected<base::Value, std::string>;

// Parses a Server-Sent Events (SSE) byte stream delivered in arbitrary chunks.
//
// A UTF-8 BOM at the start of the stream is skipped. Lines may end in CRLF, LF
// or CR. The values of consecutive `data:` lines, each with one optional
// leading space removed, are joined with "\n" and the event is dispatched on
// the next blank line. Comments and all other fields (`event:`, `id:`,
// `retry:`, ...) are ignored.
//
// Events are only dispatched when their data is a JSON object candidate, i.e.
// its first non-whitespace character is `{`. Empty data (e.g. an event carrying
// only an `id:`) and other payloads, such as the `[DONE]` sentinel sent by
// OpenAI-compatible servers, are dropped. Dispatched data is parsed as JSON
// asynchronously on `task_runner` and `on_event` receives the parsed value, or
// an error if it is not valid JSON. `on_event` may destroy the parser. Callers
// can check IsDecoding() to determine whether parse operations are still in
// flight.
class SSEParser {
 public:
  SSEParser(scoped_refptr<base::SequencedTaskRunner> task_runner,
            base::RepeatingCallback<void(ValueOrError)> on_event);
  ~SSEParser();

  SSEParser(const SSEParser&) = delete;
  SSEParser& operator=(const SSEParser&) = delete;

  // Feed the next chunk of raw SSE bytes. May dispatch async JSON parse
  // operations.
  void Process(std::string_view chunk);

  // Call at end of stream. Dispatches an event whose lines are complete but
  // which lacks the terminating blank line, then resets all buffers. An
  // unterminated final line may be truncated, so it is discarded.
  void Flush();

  // Discards all buffered input without dispatching (e.g. on retry).
  void Clear();

  // Returns true while there are JSON parse operations still in flight.
  bool IsDecoding() const;

 private:
  void ProcessLine(std::string_view line);
  void DispatchEvent();
  void OnJsonParsed(ValueOrError result);

  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::RepeatingCallback<void(ValueOrError)> on_event_;

  // Bytes after the last line ending, prepended to the next chunk.
  std::string line_buffer_;
  // `data:` values of the current event, each followed by "\n".
  std::string data_buffer_;
  // Whether the previous chunk ended with CR, so that a LF starting the next
  // chunk completes a CRLF rather than terminating an empty line.
  bool skip_leading_lf_ = false;
  // True until the start of the stream has been checked for a BOM.
  bool at_stream_start_ = true;
  size_t decoding_count_ = 0;

  base::WeakPtrFactory<SSEParser> weak_ptr_factory_{this};
};

}  // namespace api_request_helper

#endif  // BRAVE_COMPONENTS_API_REQUEST_HELPER_SSE_PARSER_H_
