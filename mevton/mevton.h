#ifndef TON_MEVTON_H
#define TON_MEVTON_H

#include "grpcpp/grpcpp.h"
#include <string>
#include <thread>

#include "auth.pb.h"
#include "auth.grpc.pb.h"

#include "block_engine.pb.h"
#include "block_engine.grpc.pb.h"

#include "searcher.pb.h"
#include "searcher.grpc.pb.h"

#include "keys/keys.hpp"
#include "validator/interfaces/external-message.h"
#include "block/transaction.h"

#include "crypto/Ed25519.h"

#include "SafeQueue.h"

class MevtonException: public std::exception {
 private:
  std::string message;
 public:
  explicit MevtonException(std::string message): message(std::move(message)) {

  }

  [[nodiscard]] const char* what() const noexcept {
    return message.c_str();
  }
};

class Mevton {
 private:
  bool enabled;
  bool stopped;
  std::shared_ptr<grpc::Channel> channel;
  td::Ed25519::PrivateKey private_key;

  std::unique_ptr<auth::Token> access_token;
  std::unique_ptr<auth::Token> refresh_token;

  std::unique_ptr<auth::AuthService::Stub> auth_service;
  std::unique_ptr<block_engine::BlockEngineValidator::Stub> block_engine_service;
  std::unique_ptr<searcher::SearcherService::Stub> searcher_service;

  SafeQueue<dto::MempoolExternalMessage> pending_mempool_messages;
  SafeQueue<dto::Bundle> pending_bundles;

  std::thread submit_messages_thread;
  std::thread fetch_pending_bundles_thread;

  static std::shared_ptr<grpc::Channel> CreateSecureChannel(const std::string& server_address) {
    auto channel_creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
    return grpc::CreateChannel(server_address, channel_creds);
  }

  auth::GenerateAuthTokensResponse GenerateAccessTokens(const auth::GenerateAuthChallengeResponse& response);
  auth::GenerateAuthChallengeResponse GenerateAuthChallenge();

 public:
  Mevton(bool enabled, const std::string& server_addr, ton::PrivateKey private_key):
      enabled(enabled),
      stopped(false),
      channel(enabled ? CreateSecureChannel(server_addr) : nullptr),
      private_key(private_key.export_as_slice())
  {
    if (enabled) {
      auth_service = auth::AuthService::NewStub(channel);
      block_engine_service = block_engine::BlockEngineValidator::NewStub(channel);
      searcher_service = searcher::SearcherService::NewStub(channel);

      submit_messages_thread = std::thread(&Mevton::SubmitMessagesWorker, this);
      fetch_pending_bundles_thread = std::thread(&Mevton::FetchPendingBundlesWorker, this);
    }
  }

  bool IsEnabled() const;

  void Authenticate();

  void SubmitExternalMessage(td::Ref<ton::validator::ExtMessage> message, std::unique_ptr<block::transaction::Transaction> transaction);

  std::list<dto::Bundle*> GetPendingBundles();

  void ResetPendingBundles();

  void SubmitMessagesWorker();
  void FetchPendingBundlesWorker();
};

#endif  //TON_MEVTON_H
