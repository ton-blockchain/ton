#include "mevton.h"
#include <google/protobuf/util/time_util.h>


void Mevton::Authenticate() {
  auto challenge = GenerateAuthChallenge();

  auto tokens = GenerateAccessTokens(challenge);

  access_token = std::make_unique<auth::Token>(tokens.access_token());
  refresh_token = std::make_unique<auth::Token>(tokens.refresh_token());
}

auth::GenerateAuthChallengeResponse Mevton::GenerateAuthChallenge() {
  auth::GenerateAuthChallengeRequest request;
  auto reply = std::make_unique<auth::GenerateAuthChallengeResponse>();

  grpc::ClientContext context;

  grpc::Status status = auth_service->GenerateAuthChallenge(&context, request, reply.get());

  if (status.ok()) {
    return *reply.get();
  } else {
    throw MevtonException("Failed to generate authentication challenge");
  }
}

auth::GenerateAuthTokensResponse Mevton::GenerateAccessTokens(const auth::GenerateAuthChallengeResponse& generate_auth_challenge_response) {
  std::string challenge = generate_auth_challenge_response.challenge();

  auto result = private_key.sign(td::Slice(challenge.data(), challenge.size()));

  if (result.is_error()) {
    throw MevtonException("Failed to sign challenge");
  }

  auth::GenerateAuthTokensRequest generate_auth_tokens_request;
  generate_auth_tokens_request.set_challenge(challenge);
  generate_auth_tokens_request.set_signed_challenge(result.ok().as_slice().str());

  auto generate_auth_tokens_response = std::make_unique<auth::GenerateAuthTokensResponse>();

  grpc::ClientContext context;

  grpc::Status status = auth_service->GenerateAuthTokens(
      &context,
      generate_auth_tokens_request,
      generate_auth_tokens_response.get()
  );

  if (status.ok()) {
    return *generate_auth_tokens_response.get();
  } else {
    throw MevtonException("Failed to generate auth tokens");
  }
}

bool Mevton::IsEnabled() const {
  return enabled;
}

void Mevton::SubmitExternalMessage(td::Ref<ton::validator::ExtMessage> message) {
  dto::MempoolMessage mempool_message;

  mempool_message.set_hash(message->hash().to_hex());
  mempool_message.set_workchain_id(message->wc());
  mempool_message.set_shard(message->shard().to_str());
  mempool_message.set_data(message->serialize().as_slice().str());
  mempool_message.set_std_smc_address(message->addr().to_hex());
//  mempool_message->set_events();
//  mempool_message->set_gas_spent();

  pending_mempool_messages.Produce(std::move(mempool_message));
}

std::list<dto::Bundle*> Mevton::GetPendingBundles() {
  std::list<dto::Bundle*> collected_bundles;

  dto::Bundle bundle;
  while(pending_bundles.Consume(bundle)) {
    collected_bundles.push_back(new dto::Bundle(bundle));
  }

  return collected_bundles;
}

void Mevton::ResetPendingBundles() {
  dto::Bundle bundle;
  while(pending_bundles.Consume(bundle)) { }
}

void Mevton::SubmitMessagesWorker() {
  block_engine::StreamMempoolResponse response;
  grpc::ClientContext context;
  auto writer = block_engine_service->StreamMempool(&context, &response);

  while (true) {
    if (stopped) {
      break;
    }

    dto::MempoolMessage pending_mempool_message;

    dto::MempoolPacket packet;

    auto current_time = google::protobuf::util::TimeUtil::GetCurrentTime();

    packet.set_allocated_server_ts(&current_time);
    packet.set_expiration_ns(5000000);

    if (pending_mempool_messages.Consume(pending_mempool_message)) {
      auto mempool_message = packet.add_messages();

      mempool_message->set_hash(pending_mempool_message.hash());
      mempool_message->set_workchain_id(pending_mempool_message.workchain_id());
      mempool_message->set_shard(pending_mempool_message.shard());
      mempool_message->set_data(pending_mempool_message.data());
      mempool_message->set_std_smc_address(pending_mempool_message.std_smc_address());
//      mempool_message->set_events(pending_mempool_message.events());
      mempool_message->set_gas_spent(pending_mempool_message.gas_spent());
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (packet.messages_size() > 0) {
      if (!writer->Write(packet)) {
        std::cerr << "Failed to write packet, restarting stream." << std::endl;
        context.TryCancel(); // Cancel the current context???
        writer = block_engine_service->StreamMempool(&context, &response);
      }
    }
  }

  writer->WritesDone();
  grpc::Status status = writer->Finish();
  if (!status.ok()) {
    std::cerr << "StreamMempool rpc failed: " << status.error_message() << std::endl;
  }
}

void Mevton::FetchPendingBundlesWorker() {
  block_engine::SubscribeBundlesRequest request;
  grpc::ClientContext context;

  std::unique_ptr<grpc::ClientReader<dto::Bundle>> reader(block_engine_service->SubscribeBundles(&context, request));

  dto::Bundle bundle;

  while (reader->Read(&bundle)) {
    if (stopped) {
      break;
    }

    pending_bundles.Produce(std::move(bundle));
  }

  grpc::Status status = reader->Finish();

  if (!status.ok()) {
    std::cerr << "StreamMempool rpc failed: " << status.error_message() << std::endl;
  }
}
