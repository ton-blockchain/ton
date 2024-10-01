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

void Mevton::SubmitExternalMessage(td::Ref<ton::validator::ExtMessage> message, std::unique_ptr<block::transaction::Transaction> transaction) {
  dto::MempoolExternalMessage mempool_message;

  mempool_message.set_hash(message->hash().to_hex());
  mempool_message.set_workchain_id(message->wc());
  mempool_message.set_shard(message->shard().to_str());
  mempool_message.set_data(message->serialize().as_slice().str());
  mempool_message.set_std_smc_address(message->addr().to_hex());
  mempool_message.set_gas_spent(transaction->gas_used());

  for (const auto& it : transaction->out_msgs) {
    std::string* msg = mempool_message.add_out_msgs();

    auto cs = load_cell_slice(it);

    msg->assign(cs.as_bitslice().to_hex());
  }

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
  std::cout << "submitting messages worker " << std::endl;
  block_engine::StreamMempoolResponse response;
  grpc::ClientContext context;
  auto writer = block_engine_service->StreamMempool(&context, &response);


  std::cout << "submitting messages worker " << std::endl;
  while (true) {
    if (stopped) {
      break;
    }

    dto::MempoolExternalMessage pending_mempool_message;

    dto::MempoolPacket packet;

    auto current_time = google::protobuf::util::TimeUtil::GetCurrentTime();

    packet.set_allocated_server_ts(&current_time);
    // @TODO: make it configurable
    packet.set_expiration_ns(2000000);

    if (pending_mempool_messages.Consume(pending_mempool_message)) {
      auto mempool_message = packet.add_external_messages();
      mempool_message->MergeFrom(pending_mempool_message);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (packet.external_messages_size() > 0) {
      if (!writer->Write(packet)) {
        std::cerr << "Failed to write packet, restarting stream." << std::endl;
        context.TryCancel(); // Cancel the current context???
        writer = block_engine_service->StreamMempool(&context, &response);
      }
    }
  }
  std::cout << "Finished to write: "<< std::endl;

  writer->WritesDone();
  grpc::Status status = writer->Finish();
  if (!status.ok()) {
    std::cerr << "-------------------------------------" << std::endl;
    std::cerr << "StreamMempool rpc failed: " << status.error_message() << std::endl;
    std::cerr << "-------------------------------------" << std::endl;
  }
}

void Mevton::FetchPendingBundlesWorker() {
  std::cout << "fetching pending bundles worker " << std::endl;
 block_engine::SubscribeBundlesRequest request;
  grpc::ClientContext context;

  std::unique_ptr<grpc::ClientReader<dto::Bundle>> reader(block_engine_service->SubscribeBundles(&context, request));

  dto::Bundle bundle;

  std::cout <<"Starting to read bundles" <<std::endl;
  while (reader->Read(&bundle)) {
    if (stopped) {
      break;
    }

    pending_bundles.Produce(std::move(bundle));
  }

  std::cout << "Pending bundles to add: " <<pending_bundles.size() << std::endl;
  grpc::Status status = reader->Finish();

  if (!status.ok()) {
    std::cerr << "-------------------------------------" << std::endl;
    std::cerr << "StreamMempool rpc failed: " << status.error_message() << std::endl;
    std::cerr << "-------------------------------------" << std::endl;

  }
}
