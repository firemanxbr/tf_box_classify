/* =============================================================================
Example usage:
  tf_convnet_inference --port=9000 /tmp/mnist_model/00000001
==============================================================================*/

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "grpc++/security/server_credentials.h"
#include "grpc++/server.h"
#include "grpc++/server_builder.h"
#include "grpc++/server_context.h"
#include "grpc++/support/status.h"
#include "grpc++/support/status_code_enum.h"
#include "grpc/grpc.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_types.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/init_main.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/public/session.h"
#include "tensorflow/core/public/session_options.h"
#include "tensorflow/core/util/command_line_flags.h"
#include "tensorflow_serving/convnet_test/tf_convnet_inference.grpc.pb.h"
#include "tensorflow_serving/convnet_test/tf_convnet_inference.pb.h"
#include "tensorflow_serving/servables/tensorflow/session_bundle_config.pb.h"
#include "tensorflow_serving/servables/tensorflow/session_bundle_factory.h"
#include "tensorflow_serving/session_bundle/manifest.pb.h"
#include "tensorflow_serving/session_bundle/session_bundle.h"
#include "tensorflow_serving/session_bundle/signature.h"

using grpc::InsecureServerCredentials;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::StatusCode;
using tensorflow::serving::ClassificationSignature;
using tensorflow::serving::BoxImageRequest;
using tensorflow::serving::BoxImageResponse;
using tensorflow::serving::BoxImageService;
using tensorflow::serving::BatchingParameters;
using tensorflow::serving::SessionBundle;
using tensorflow::serving::SessionBundleConfig;
using tensorflow::serving::SessionBundleFactory;
using tensorflow::string;
using tensorflow::Tensor;
using tensorflow::TensorShape;

namespace {
const int kImageSize = 150;
const int kNumChannels = 1;
const int kImageDataSize = kImageSize * kImageSize * kNumChannels;
const int kNumLabels = 4;

// Creates a gRPC Status from a TensorFlow Status.
Status ToGRPCStatus(const tensorflow::Status& status) {
  return Status(static_cast<grpc::StatusCode>(status.code()),
                status.error_message());
}

class BoxImageServiceImpl final : public BoxImageService::Service {
 public:
  explicit BoxImageServiceImpl(std::unique_ptr<SessionBundle> bundle)
      : bundle_(std::move(bundle)) {
    signature_status_ = tensorflow::serving::GetClassificationSignature(
        bundle_->meta_graph_def, &signature_);
  }

  Status Classify(ServerContext* context, const BoxImageRequest* request,
                  BoxImageResponse* response) override {
    // Verify protobuf input.
    if (request->image_data_size() != kImageDataSize) {
      return Status(StatusCode::INVALID_ARGUMENT,
                    tensorflow::strings::StrCat("expected image_data of size ",
                                                kImageDataSize, ", got ",
                                                request->image_data_size()));
    }

    // Transform protobuf input to inference input tensor and create
    // output tensor placeholder.
    Tensor input(tensorflow::DT_FLOAT, {1, kImageDataSize});
    std::copy_n(request->image_data().begin(), kImageDataSize,
                input.flat<float>().data());
    std::vector<Tensor> outputs;

    // Run inference.
    if (!signature_status_.ok()) {
      return ToGRPCStatus(signature_status_);
    }
    // WARNING(break-tutorial-inline-code): The following code snippet is
    // in-lined in tutorials, please update tutorial documents accordingly
    // whenever code changes.
    const tensorflow::Status status = bundle_->session->Run(
        {{signature_.input().tensor_name(), input}},
        {signature_.scores().tensor_name()}, {}, &outputs);
    if (!status.ok()) {
      return ToGRPCStatus(status);
    }

    // Transform inference output tensor to protobuf output.
    if (outputs.size() != 1) {
      return Status(StatusCode::INTERNAL,
                    tensorflow::strings::StrCat(
                        "expected one model output, got ", outputs.size()));
    }
    const Tensor& score_tensor = outputs[0];
    const TensorShape expected_shape({1, kNumLabels});
    if (!score_tensor.shape().IsSameSize(expected_shape)) {
      return Status(
          StatusCode::INTERNAL,
          tensorflow::strings::StrCat("expected output of size ",
                                      expected_shape.DebugString(), ", got ",
                                      score_tensor.shape().DebugString()));
    }
    const auto score_flat = outputs[0].flat<float>();
    for (int i = 0; i < score_flat.size(); ++i) {
      response->add_value(score_flat(i));
    }

    return Status::OK;
  }

 private:
  std::unique_ptr<SessionBundle> bundle_;
  tensorflow::Status signature_status_;
  ClassificationSignature signature_;
};

void RunServer(int port, std::unique_ptr<SessionBundle> bundle) {
  // "0.0.0.0" is the way to listen on localhost in gRPC.
  const string server_address = "0.0.0.0:" + std::to_string(port);
  BoxImageServiceImpl service(std::move(bundle));
  ServerBuilder builder;
  std::shared_ptr<grpc::ServerCredentials> creds = InsecureServerCredentials();
  builder.AddListeningPort(server_address, creds);
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  LOG(INFO) << "Running...";
  server->Wait();
}

}  // namespace

int main(int argc, char** argv) {
  tensorflow::int32 port = 0;
  const bool parse_result =
      tensorflow::ParseFlags(&argc, argv, {tensorflow::Flag("port", &port)});
  if (!parse_result) {
    LOG(FATAL) << "Error parsing command line flags.";
  }

  if (argc != 2) {
    LOG(FATAL) << "Usage: tf_convnet_inference --port=9000 /path/to/export";
  }
  const string bundle_path(argv[1]);

  tensorflow::port::InitMain(argv[0], &argc, &argv);

  // WARNING(break-tutorial-inline-code): The following code snippet is
  // in-lined in tutorials, please update tutorial documents accordingly
  // whenever code changes.

  SessionBundleConfig session_bundle_config;

  //////
  // Request batching, keeping default values for the tuning parameters.
  //
  // (If you prefer to disable batching, simply omit the following lines of code
  // such that session_bundle_config.batching_parameters remains unset.)
  BatchingParameters* batching_parameters =
      session_bundle_config.mutable_batching_parameters();
  batching_parameters->mutable_thread_pool_name()->set_value(
      "box_image_service_batch_threads");
  //////

  std::unique_ptr<SessionBundleFactory> bundle_factory;
  TF_QCHECK_OK(
      SessionBundleFactory::Create(session_bundle_config, &bundle_factory));
  std::unique_ptr<SessionBundle> bundle(new SessionBundle);
  TF_QCHECK_OK(bundle_factory->CreateSessionBundle(bundle_path, &bundle));

  // END WARNING(break-tutorial-inline-code)

  RunServer(port, std::move(bundle));

  return 0;
}
