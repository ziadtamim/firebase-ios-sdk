/*
 * Copyright 2018 Google
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Firestore/core/test/firebase/firestore/util/grpc_stream_tester.h"

#include <sstream>
#include <utility>
#include <vector>

#include "Firestore/core/src/firebase/firestore/util/string_format.h"
#include "absl/memory/memory.h"

namespace firebase {
namespace firestore {
namespace util {

using auth::Token;
using auth::User;
using internal::ExecutorStd;
using model::DatabaseId;
using remote::ConnectivityMonitor;
using remote::GrpcCompletion;
using remote::GrpcStream;
using remote::GrpcStreamingReader;
using remote::GrpcStreamObserver;
using util::CompletionEndState;

// Misc

std::string GetGrpcErrorCodeName(grpc::StatusCode error) {
  switch (error) {
    case grpc::OK:
      return "Ok";
    case grpc::CANCELLED:
      return "Cancelled";
    case grpc::UNKNOWN:
      return "Unknown";
    case grpc::INVALID_ARGUMENT:
      return "InvalidArgument";
    case grpc::DEADLINE_EXCEEDED:
      return "DeadlineExceeded";
    case grpc::NOT_FOUND:
      return "NotFound";
    case grpc::ALREADY_EXISTS:
      return "AlreadyExists";
    case grpc::PERMISSION_DENIED:
      return "PermissionDenied";
    case grpc::RESOURCE_EXHAUSTED:
      return "ResourceExhausted";
    case grpc::FAILED_PRECONDITION:
      return "FailedPrecondition";
    case grpc::ABORTED:
      return "Aborted";
    case grpc::OUT_OF_RANGE:
      return "OutOfRange";
    case grpc::UNIMPLEMENTED:
      return "Unimplemented";
    case grpc::INTERNAL:
      return "Internal";
    case grpc::UNAVAILABLE:
      return "Unavailable";
    case grpc::DATA_LOSS:
      return "DataLoss";
    case grpc::UNAUTHENTICATED:
      return "Unauthenticated";
    default:
      HARD_FAIL(StringFormat("Unexpected error code: '%s'", error).c_str());
  }
}

std::string GetFirestoreErrorCodeName(FirestoreErrorCode error) {
  return GetGrpcErrorCodeName(static_cast<grpc::StatusCode>(error));
}

std::string ByteBufferToString(const grpc::ByteBuffer& buffer) {
  std::vector<grpc::Slice> slices;
  grpc::Status status = buffer.Dump(&slices);

  std::stringstream output;
  for (const auto& slice : slices) {
    for (uint8_t c : slice) {
      output << static_cast<char>(c);
    }
  }

  return output.str();
}

grpc::ByteBuffer MakeByteBuffer(const std::string& str) {
  grpc::Slice slice{str};
  return grpc::ByteBuffer(&slice, 1);
}

// CompletionEndState

void CompletionEndState::Apply(GrpcCompletion* completion) {
  HARD_ASSERT(completion->type() == type_,
              StringFormat(
                  "Expected GrpcCompletion to be of type '%s', but it was '%s'",
                  type_, completion->type())
                  .c_str());

  if (maybe_message_) {
    *completion->message() = maybe_message_.value();
  }
  if (maybe_status_) {
    *completion->status() = maybe_status_.value();
  }

  completion->Complete(result_ == CompletionResult::Ok);
}

// FakeGrpcQueue

FakeGrpcQueue::FakeGrpcQueue(grpc::CompletionQueue* grpc_queue)
    : grpc_queue_{grpc_queue},
      dedicated_executor_{absl::make_unique<ExecutorStd>()} {
}

void FakeGrpcQueue::Shutdown() {
  if (is_shut_down_) {
    return;
  }
  is_shut_down_ = true;

  grpc_queue_->Shutdown();
  // Wait for gRPC completion queue to drain
  dedicated_executor_->ExecuteBlocking([] {});
}

GrpcCompletion* FakeGrpcQueue::ExtractCompletion() {
  HARD_ASSERT(
      dedicated_executor_->IsCurrentExecutor(),
      "gRPC completion queue must only be polled on the dedicated executor");
  bool ignored_ok = false;
  void* tag = nullptr;
  grpc_queue_->Next(&tag, &ignored_ok);
  return static_cast<GrpcCompletion*>(tag);
}

void FakeGrpcQueue::ExtractCompletions(
    std::initializer_list<CompletionEndState> results) {
  dedicated_executor_->ExecuteBlocking([&] {
    for (CompletionEndState end_state : results) {
      end_state.Apply(ExtractCompletion());
    }
  });
}

void FakeGrpcQueue::ExtractCompletions(const CompletionCallback& callback) {
  dedicated_executor_->ExecuteBlocking([&] {
    bool done = false;
    while (!done) {
      auto* completion = ExtractCompletion();
      done = callback(completion);
    }
  });
}

void FakeGrpcQueue::KeepPolling() {
  dedicated_executor_->Execute([&] {
    void* tag = nullptr;
    bool ignored_ok = false;
    while (grpc_queue_->Next(&tag, &ignored_ok)) {
      static_cast<GrpcCompletion*>(tag)->Complete(true);
    }
  });
}

// GrpcStreamTester

GrpcStreamTester::GrpcStreamTester(AsyncQueue* worker_queue,
                                   ConnectivityMonitor* connectivity_monitor)
    : worker_queue_{NOT_NULL(worker_queue)},
      database_info_{DatabaseId{"foo", "bar"}, "", "", false},
      fake_grpc_queue_{&grpc_queue_},
      grpc_connection_{database_info_, worker_queue, fake_grpc_queue_.queue(),
                       connectivity_monitor} {
}

GrpcStreamTester::~GrpcStreamTester() {
  // Make sure the stream and gRPC completion queue are properly shut down.
  Shutdown();
}

void GrpcStreamTester::Shutdown() {
  worker_queue_->EnqueueBlocking([&] { ShutdownGrpcQueue(); });
}

std::unique_ptr<GrpcStream> GrpcStreamTester::CreateStream(
    GrpcStreamObserver* observer) {
  return grpc_connection_.CreateStream("", Token{"", User{}}, observer);
}

std::unique_ptr<GrpcStreamingReader> GrpcStreamTester::CreateStreamingReader() {
  return grpc_connection_.CreateStreamingReader("", Token{"", User{}},
                                                grpc::ByteBuffer{});
}

std::unique_ptr<remote::GrpcUnaryCall> GrpcStreamTester::CreateUnaryCall() {
  return grpc_connection_.CreateUnaryCall("", Token{"", User{}},
                                          grpc::ByteBuffer{});
}

void GrpcStreamTester::ShutdownGrpcQueue() {
  fake_grpc_queue_.Shutdown();
}

// This is a very hacky way to simulate gRPC finishing operations without
// actually connecting to the server: cancel the stream, which will make all
// operations fail fast and be returned from the completion queue, then
// complete the associated completion.
void GrpcStreamTester::ForceFinish(
    grpc::ClientContext* context,
    std::initializer_list<CompletionEndState> end_states) {
  // gRPC allows calling `TryCancel` more than once.
  context->TryCancel();
  fake_grpc_queue_.ExtractCompletions(end_states);
  worker_queue_->EnqueueBlocking([] {});
}

void GrpcStreamTester::ForceFinish(grpc::ClientContext* context,
                                   const CompletionCallback& callback) {
  // gRPC allows calling `TryCancel` more than once.
  context->TryCancel();
  fake_grpc_queue_.ExtractCompletions(callback);
  worker_queue_->EnqueueBlocking([] {});
}

void GrpcStreamTester::KeepPollingGrpcQueue() {
  fake_grpc_queue_.KeepPolling();
}

}  // namespace util
}  // namespace firestore
}  // namespace firebase