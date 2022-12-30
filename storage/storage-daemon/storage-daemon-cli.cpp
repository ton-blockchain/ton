/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/bitstring.h"
#include "keys/encryptor.h"
#include "adnl/adnl-ext-client.h"

#include "td/utils/port/signals.h"
#include "td/utils/Parser.h"
#include "td/utils/OptionParser.h"
#include "td/utils/PathView.h"
#include "td/utils/misc.h"
#include "td/utils/filesystem.h"
#include "td/utils/port/path.h"

#include "td/actor/MultiPromise.h"
#include "terminal/terminal.h"

#include "auto/tl/ton_api_json.h"

#include <iostream>
#include <cstring>
#include <cstdio>
#include <set>
#include "git.h"
#include "common/refint.h"
#include "crypto/block/block.h"

namespace ton {

bool is_whitespace(char c) {
  return strchr(" \t\n\r", c) != nullptr;
};

td::Result<std::vector<std::string>> tokenize(td::Slice s) {
  const char* ptr = s.begin();
  auto skip_ws = [&]() {
    while (ptr != s.end() && is_whitespace(*ptr)) {
      ++ptr;
    }
  };
  std::vector<std::string> tokens;
  while (true) {
    skip_ws();
    if (ptr == s.end()) {
      break;
    }
    char quote = '\0';
    if (*ptr == '"' || *ptr == '\'') {
      quote = *ptr;
      ++ptr;
    }
    std::string token;
    while (true) {
      if (ptr == s.end()) {
        if (quote) {
          return td::Status::Error("Unmatched quote");
        }
        break;
      } else if (*ptr == '\\') {
        ++ptr;
        if (ptr == s.end()) {
          return td::Status::Error("Backslash at the end of the line");
        }
        switch (*ptr) {
          case 'n':
            token += '\n';
            break;
          case 't':
            token += '\t';
            break;
          case 'r':
            token += '\r';
            break;
          default:
            token += *ptr;
        }
        ++ptr;
      } else if (*ptr == quote || (!quote && is_whitespace(*ptr))) {
        ++ptr;
        break;
      } else {
        token += *ptr++;
      }
    }
    tokens.push_back(token);
  }
  return tokens;
}

std::string size_to_str(td::uint64 size) {
  td::StringBuilder s;
  s << td::format::as_size(size);
  return s.as_cslice().str();
}

std::string time_to_str(td::uint32 time) {
  char time_buffer[80];
  time_t rawtime = time;
  struct tm tInfo;
#if defined(_WIN32) || defined(_WIN64)
  struct tm* timeinfo = localtime_s(&tInfo, &rawtime) ? nullptr : &tInfo;
#else
  struct tm* timeinfo = localtime_r(&rawtime, &tInfo);
#endif
  assert(timeinfo == &tInfo);
  strftime(time_buffer, 80, "%c", timeinfo);
  return time_buffer;
}

std::string coins_to_str(const td::RefInt256& x) {
  if (x.is_null() || x->sgn() < 0) {
    return "???";
  }
  char buf[12];
  snprintf(buf, sizeof(buf), ".%09llu", (x % td::make_refint(1'000'000'000))->to_long());
  return (x / td::make_refint(1'000'000'000))->to_dec_string() + buf;
}

std::string coins_to_str(const std::string& s) {
  return coins_to_str(td::string_to_int256(s));
}

template <typename T>
void print_json(const tl_object_ptr<T>& obj) {
  std::string s = td::json_encode<std::string>(td::ToJson(*obj), true);
  td::TerminalIO::out() << s << "\n";
}

void print_table(const std::vector<std::vector<std::string>>& table, std::set<size_t> left_cols = {}) {
  if (table.empty()) {
    return;
  }
  size_t cols = table[0].size();
  std::vector<size_t> col_size(cols, 0);
  for (const auto& row : table) {
    CHECK(row.size() == cols);
    for (size_t i = 0; i < cols; ++i) {
      col_size[i] = std::max(col_size[i], row[i].size());
    }
  }
  for (const auto& row : table) {
    std::string row_str;
    for (size_t i = 0; i < cols; ++i) {
      if (i != 0) {
        row_str += "  ";
      }
      size_t pad = col_size[i] - row[i].size();
      if (!left_cols.count(i)) {
        while (pad--) {
          row_str += ' ';
        }
      }
      row_str += row[i];
      if (left_cols.count(i)) {
        while (pad--) {
          row_str += ' ';
        }
      }
    }
    td::TerminalIO::out() << row_str << "\n";
  }
}

struct OptionalProviderParams {
  td::optional<bool> accept_new_contracts;
  td::optional<std::string> rate_per_mb_day;
  td::optional<td::uint32> max_span;
  td::optional<td::uint64> minimal_file_size;
  td::optional<td::uint64> maximal_file_size;
};

struct OptionalProviderConfig {
  td::optional<td::uint32> max_contracts;
  td::optional<td::uint64> max_total_size;
};

class StorageDaemonCli : public td::actor::Actor {
 public:
  explicit StorageDaemonCli(td::IPAddress server_ip, PrivateKey client_private_key, PublicKey server_public_key,
                            std::vector<std::string> commands)
      : server_ip_(server_ip)
      , client_private_key_(client_private_key)
      , server_public_key_(server_public_key)
      , commands_(std::move(commands))
      , batch_mode_(!commands_.empty()) {
  }

  void start_up() override {
    class ExtClientCallback : public adnl::AdnlExtClient::Callback {
     public:
      explicit ExtClientCallback(td::actor::ActorId<StorageDaemonCli> id) : id_(id) {
      }
      void on_ready() override {
        LOG(INFO) << "Connected";
        td::actor::send_closure(id_, &StorageDaemonCli::on_conn_status, true);
      }
      void on_stop_ready() override {
        LOG(WARNING) << "Connection closed";
        td::actor::send_closure(id_, &StorageDaemonCli::on_conn_status, false);
      }

     private:
      td::actor::ActorId<StorageDaemonCli> id_;
    };
    CHECK(server_ip_.is_valid());
    client_ = adnl::AdnlExtClient::create(adnl::AdnlNodeIdFull{server_public_key_}, client_private_key_, server_ip_,
                                          std::make_unique<ExtClientCallback>(actor_id(this)));

    if (!batch_mode_) {
      class TerminalCallback : public td::TerminalIO::Callback {
       public:
        void line_cb(td::BufferSlice line) override {
          td::actor::send_closure(id_, &StorageDaemonCli::parse_line, std::move(line));
        }
        TerminalCallback(td::actor::ActorId<StorageDaemonCli> id) : id_(std::move(id)) {
        }

       private:
        td::actor::ActorId<StorageDaemonCli> id_;
      };
      io_ = td::TerminalIO::create("> ", true, false, std::make_unique<TerminalCallback>(actor_id(this)));
      td::actor::send_closure(io_, &td::TerminalIO::set_log_interface);
    }
  }

  void on_conn_status(bool status) {
    if (batch_mode_ && !batch_started_) {
      batch_started_ = true;
      parse_line(td::BufferSlice(commands_[cur_command_++]));
    }
  }

  void parse_line(td::BufferSlice line) {
    td::Status S = parse_line_impl(std::move(line));
    if (S.is_error()) {
      command_finished(std::move(S));
    }
  }

  td::Status parse_line_impl(td::BufferSlice line) {
    auto parse_hash = [](const std::string& s) -> td::Result<td::Bits256> {
      td::Bits256 hash;
      if (hash.from_hex(s) != 256) {
        return td::Status::Error("Invalid BagID");
      }
      return hash;
    };
    auto parse_torrent = [&](const std::string& s) -> td::Result<td::Bits256> {
      if (s.length() == 64) {
        return parse_hash(s);
      }
      if (batch_mode_) {
        return td::Status::Error("Indices are not available in batch mode");
      }
      TRY_RESULT(id, td::to_integer_safe<td::uint32>(s));
      auto it = id_to_hash_.find(id);
      if (it == id_to_hash_.end()) {
        return td::Status::Error(PSTRING() << "Unknown index " << id);
      }
      return it->second;
    };

    TRY_RESULT_PREFIX(tokens, tokenize(line), "Failed to parse line: ");
    if (tokens.empty()) {
      command_finished(td::Status::OK());
      return td::Status::OK();
    }
    if (tokens[0] == "quit" || tokens[0] == "exit") {
      if (tokens.size() != 1) {
        return td::Status::Error("Unexpected tokens");
      }
      std::_Exit(0);
    } else if (tokens[0] == "help") {
      if (tokens.size() != 1) {
        return td::Status::Error("Unexpected tokens");
      }
      return execute_help();
    } else if (tokens[0] == "setverbosity") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected level");
      }
      TRY_RESULT_PREFIX(level, td::to_integer_safe<int>(tokens[1]), "Invalid level: ");
      return execute_set_verbosity(level);
    } else if (tokens[0] == "create") {
      std::string path;
      bool found_path = false;
      bool upload = true;
      bool copy = false;
      std::string description;
      bool json = false;
      bool no_more_flags = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-' && !no_more_flags) {
          if (tokens[i] == "-d") {
            ++i;
            if (i == tokens.size()) {
              return td::Status::Error("Unexpected EOLN");
            }
            description = tokens[i];
            continue;
          }
          if (tokens[i] == "--no-upload") {
            upload = false;
            continue;
          }
          if (tokens[i] == "--copy") {
            copy = true;
            continue;
          }
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          if (tokens[i] == "--") {
            no_more_flags = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (found_path) {
          return td::Status::Error("Unexpected token");
        }
        path = tokens[i];
        found_path = true;
      }
      if (!found_path) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_create(std::move(path), std::move(description), upload, copy, json);
    } else if (tokens[0] == "add-by-hash" || tokens[0] == "add-by-meta") {
      td::optional<std::string> param;
      std::string root_dir;
      bool paused = false;
      bool upload = true;
      bool json = false;
      td::optional<std::vector<std::string>> partial;
      bool no_more_flags = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-' && !no_more_flags) {
          if (tokens[i] == "-d") {
            ++i;
            if (i == tokens.size()) {
              return td::Status::Error("Unexpected EOLN");
            }
            root_dir = tokens[i];
            continue;
          }
          if (tokens[i] == "--paused") {
            paused = true;
            continue;
          }
          if (tokens[i] == "--no-upload") {
            upload = false;
            continue;
          }
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          if (tokens[i] == "--partial") {
            partial = std::vector<std::string>(tokens.begin() + i + 1, tokens.end());
            break;
          }
          if (tokens[i] == "--") {
            no_more_flags = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (param) {
          return td::Status::Error("Unexpected token");
        }
        param = tokens[i];
      }
      if (!param) {
        return td::Status::Error("Unexpected EOLN");
      }
      if (tokens[0] == "add-by-hash") {
        TRY_RESULT(hash, parse_hash(param.value()));
        return execute_add_by_hash(hash, std::move(root_dir), paused, upload, std::move(partial), json);
      } else {
        return execute_add_by_meta(param.value(), std::move(root_dir), paused, upload, std::move(partial), json);
      }
    } else if (tokens[0] == "list") {
      bool with_hashes = false;
      bool json = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "--hashes") {
          with_hashes = true;
          continue;
        }
        if (tokens[i] == "--json") {
          json = true;
          continue;
        }
        return td::Status::Error(PSTRING() << "Unexpected argument " << tokens[i]);
      }
      return execute_list(with_hashes, json);
    } else if (tokens[0] == "get") {
      td::Bits256 hash;
      bool found_hash = false;
      bool json = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (found_hash) {
          return td::Status::Error("Unexpected token");
        }
        TRY_RESULT_ASSIGN(hash, parse_torrent(tokens[i]));
        found_hash = true;
      }
      if (!found_hash) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_get(hash, json);
    } else if (tokens[0] == "get-meta") {
      if (tokens.size() != 3) {
        return td::Status::Error("Expected bag and file");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      return execute_get_meta(hash, tokens[2]);
    } else if (tokens[0] == "get-peers") {
      td::Bits256 hash;
      bool found_hash = false;
      bool json = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (found_hash) {
          return td::Status::Error("Unexpected token");
        }
        TRY_RESULT_ASSIGN(hash, parse_torrent(tokens[i]));
        found_hash = true;
      }
      if (!found_hash) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_get_peers(hash, json);
    } else if (tokens[0] == "download-pause" || tokens[0] == "download-resume") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected bag");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      return execute_set_active_download(hash, tokens[0] == "download-resume");
    } else if (tokens[0] == "upload-pause" || tokens[0] == "upload-resume") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected bag");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      return execute_set_active_upload(hash, tokens[0] == "upload-resume");
    } else if (tokens[0] == "priority-all") {
      if (tokens.size() != 3) {
        return td::Status::Error("Expected bag and priority");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      TRY_RESULT_PREFIX(priority, td::to_integer_safe<td::uint8>(tokens[2]), "Invalid priority: ");
      return execute_set_priority_all(hash, priority);
    } else if (tokens[0] == "priority-idx") {
      if (tokens.size() != 4) {
        return td::Status::Error("Expected bag, idx and priority");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      TRY_RESULT_PREFIX(idx, td::to_integer_safe<td::uint64>(tokens[2]), "Invalid idx: ");
      TRY_RESULT_PREFIX(priority, td::to_integer_safe<td::uint8>(tokens[3]), "Invalid priority: ");
      return execute_set_priority_idx(hash, idx, priority);
    } else if (tokens[0] == "priority-name") {
      if (tokens.size() != 4) {
        return td::Status::Error("Expected bag, name and priority");
      }
      TRY_RESULT(hash, parse_torrent(tokens[1]));
      TRY_RESULT_PREFIX(priority, td::to_integer_safe<td::uint8>(tokens[3]), "Invalid priority: ");
      return execute_set_priority_name(hash, tokens[2], priority);
    } else if (tokens[0] == "remove") {
      td::Bits256 hash;
      bool found_hash = false;
      bool remove_files = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--remove-files") {
            remove_files = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (found_hash) {
          return td::Status::Error("Unexpected token");
        }
        TRY_RESULT_ASSIGN(hash, parse_torrent(tokens[i]));
        found_hash = true;
      }
      if (!found_hash) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_remove(hash, remove_files);
    } else if (tokens[0] == "load-from") {
      td::Bits256 hash;
      std::string meta, path;
      bool found_hash = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--meta") {
            ++i;
            meta = tokens[i];
            continue;
          }
          if (tokens[i] == "--files") {
            ++i;
            path = tokens[i];
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (found_hash) {
          return td::Status::Error("Unexpected token");
        }
        TRY_RESULT_ASSIGN(hash, parse_torrent(tokens[i]));
        found_hash = true;
      }
      if (!found_hash) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_load_from(hash, std::move(meta), std::move(path));
    } else if (tokens[0] == "new-contract-message") {
      td::Bits256 hash;
      std::string file;
      td::uint64 query_id = 0;
      int cnt = 0;
      td::optional<std::string> provider_address;
      td::optional<std::string> rate;
      td::optional<td::uint32> max_span;
      bool no_more_flags = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-' && !no_more_flags) {
          if (tokens[i] == "--query-id") {
            ++i;
            TRY_RESULT_PREFIX_ASSIGN(query_id, td::to_integer_safe<td::uint64>(tokens[i]), "Invalid query id: ");
            continue;
          }
          if (tokens[i] == "--provider") {
            ++i;
            provider_address = tokens[i];
            continue;
          }
          if (tokens[i] == "--rate") {
            ++i;
            rate = tokens[i];
            continue;
          }
          if (tokens[i] == "--max-span") {
            ++i;
            TRY_RESULT_PREFIX_ASSIGN(max_span, td::to_integer_safe<td::uint8>(tokens[i]), "Invalid max span: ");
            continue;
          }
          if (tokens[i] == "--") {
            no_more_flags = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (cnt == 0) {
          TRY_RESULT_ASSIGN(hash, parse_torrent(tokens[i]));
        } else if (cnt == 1) {
          file = tokens[i];
        }
        ++cnt;
      }
      if (cnt != 2) {
        return td::Status::Error("Unexpected EOLN");
      }
      return execute_new_contract_message(hash, std::move(file), query_id, std::move(provider_address), std::move(rate),
                                          max_span);
    } else if (tokens[0] == "import-pk") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected filename");
      }
      return execute_import_pk(tokens[1]);
    } else if (tokens[0] == "get-provider-params") {
      bool json = false;
      std::string address;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (!address.empty()) {
          return td::Status::Error("Unexpected token");
        }
        address = tokens[i];
      }
      return execute_get_provider_params(address, json);
    } else if (tokens[0] == "deploy-provider") {
      if (tokens.size() != 1) {
        return td::Status::Error("Unexpected tokens");
      }
      return execute_deploy_provider();
    } else if (tokens[0] == "init-provider") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected address");
      }
      return execute_init_provider(tokens[1]);
    } else if (tokens[0] == "remove-storage-provider") {
      if (tokens.size() != 1) {
        return td::Status::Error("Unexpected tokens");
      }
      return execute_remove_storage_provider();
    } else if (tokens[0] == "set-provider-params") {
      if (tokens.size() == 1) {
        return td::Status::Error("No parameters specified");
      }
      if (tokens.size() % 2 == 0) {
        return td::Status::Error("Unexpected number of tokens");
      }
      OptionalProviderParams new_params;
      for (size_t i = 1; i < tokens.size(); i += 2) {
        if (tokens[i] == "--accept") {
          if (tokens[i + 1] == "0") {
            new_params.accept_new_contracts = false;
          } else if (tokens[i + 1] == "1") {
            new_params.accept_new_contracts = true;
          } else {
            return td::Status::Error("Invalid value for --accept");
          }
          continue;
        }
        if (tokens[i] == "--rate") {
          new_params.rate_per_mb_day = tokens[i + 1];
          continue;
        }
        if (tokens[i] == "--max-span") {
          TRY_RESULT_PREFIX(x, td::to_integer_safe<td::uint32>(tokens[i + 1]), "Invalid value for --max-span: ");
          new_params.max_span = x;
          continue;
        }
        if (tokens[i] == "--min-file-size") {
          TRY_RESULT_PREFIX(x, td::to_integer_safe<td::uint64>(tokens[i + 1]), "Invalid value for --min-file-size: ");
          new_params.minimal_file_size = x;
          continue;
        }
        if (tokens[i] == "--max-file-size") {
          TRY_RESULT_PREFIX(x, td::to_integer_safe<td::uint64>(tokens[i + 1]), "Invalid value for --max-file-size: ");
          new_params.maximal_file_size = x;
          continue;
        }
        return td::Status::Error(PSTRING() << "Unexpected token " << tokens[i]);
      }
      return execute_set_provider_params(std::move(new_params));
    } else if (tokens[0] == "get-provider-info") {
      bool with_balances = false;
      bool with_contracts = false;
      bool json = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--balances") {
            with_balances = true;
            continue;
          }
          if (tokens[i] == "--contracts") {
            with_contracts = true;
            continue;
          }
          if (tokens[i] == "--json") {
            json = true;
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
      }
      return execute_get_provider_info(with_balances, with_contracts, json);
    } else if (tokens[0] == "set-provider-config") {
      if (tokens.size() == 1) {
        return td::Status::Error("No parameters specified");
      }
      if (tokens.size() % 2 == 0) {
        return td::Status::Error("Unexpected number of tokens");
      }
      OptionalProviderConfig new_config;
      for (size_t i = 1; i < tokens.size(); i += 2) {
        if (tokens[i] == "--max-contracts") {
          TRY_RESULT_PREFIX(x, td::to_integer_safe<td::uint32>(tokens[i + 1]), "Invalid value for --max-contracts: ");
          new_config.max_contracts = x;
          continue;
        }
        if (tokens[i] == "--max-total-size") {
          TRY_RESULT_PREFIX(x, td::to_integer_safe<td::uint64>(tokens[i + 1]), "Invalid value for --max-total-size: ");
          new_config.max_total_size = x;
          continue;
        }
        return td::Status::Error(PSTRING() << "Unexpected token " << tokens[i]);
      }
      return execute_set_provider_config(std::move(new_config));
    } else if (tokens[0] == "withdraw") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected contract address");
      }
      return execute_withdraw(tokens[1]);
    } else if (tokens[0] == "withdraw-all") {
      if (tokens.size() != 1) {
        return td::Status::Error("Unexpected tokens");
      }
      return execute_withdraw_all();
    } else if (tokens[0] == "send-coins") {
      std::string address;
      std::string amount;
      int cnt = 0;
      std::string message;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "--message") {
            ++i;
            if (i == tokens.size()) {
              return td::Status::Error("Expected message");
            }
            message = tokens[i];
            continue;
          }
          return td::Status::Error(PSTRING() << "Unknown flag " << tokens[i]);
        }
        if (cnt == 0) {
          address = tokens[i];
        } else if (cnt == 1) {
          amount = tokens[i];
        } else {
          return td::Status::Error("Expected address and amount");
        }
        ++cnt;
      }
      if (cnt != 2) {
        return td::Status::Error("Expected address and amount");
      }
      return execute_send_coins(address, amount, message);
    } else if (tokens[0] == "close-contract") {
      if (tokens.size() != 2) {
        return td::Status::Error("Expected address");
      }
      return execute_close_contract(tokens[1]);
    } else {
      return td::Status::Error(PSTRING() << "Error: unknown command " << tokens[0]);
    }
  }

  td::Status execute_help() {
    td::TerminalIO::out() << "help\tPrint this help\n";
    td::TerminalIO::out()
        << "create [-d description] [--no-upload] [--copy] [--json] <file/dir>\tCreate bag of files from <file/dir>\n";
    td::TerminalIO::out() << "\t-d\tDescription will be stored in torrent info\n";
    td::TerminalIO::out() << "\t--no-upload\tDon't share bag with peers\n";
    td::TerminalIO::out() << "\t--copy\tFiles will be copied to an internal directory of storage-daemon\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "add-by-hash <bag-id> [-d root_dir] [--paused] [--no-upload] [--json] [--partial file1 "
                             "file2 ...]\tAdd bag with given BagID (in hex)\n";
    td::TerminalIO::out() << "\t-d\tTarget directory, default is an internal directory of storage-daemon\n";
    td::TerminalIO::out() << "\t--paused\tDon't start download immediately\n";
    td::TerminalIO::out() << "\t--no-upload\tDon't share bag with peers\n";
    td::TerminalIO::out()
        << "\t--partial\tEverything after this flag is a list of filenames. Only these files will be downloaded.\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "add-by-meta <meta> [-d root_dir] [--paused] [--no-upload] [--json] [--partial file1 "
                             "file2 ...]\tLoad meta from file and add bag\n";
    td::TerminalIO::out() << "\tFlags are the same as in add-by-hash\n";
    td::TerminalIO::out() << "list [--hashes] [--json]\tPrint list of bags\n";
    td::TerminalIO::out() << "\t--hashes\tPrint full BagID\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "get <bag> [--json]\tPrint information about <bag>\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "\tHere and below bags are identified by BagID (in hex) or index (see bag list)\n";
    td::TerminalIO::out() << "get-meta <bag> <file>\tSave bag meta of <bag> to <file>\n";
    td::TerminalIO::out() << "get-peers <bag> [--json]\tPrint a list of peers\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "download-pause <bag>\tPause download of <bag>\n";
    td::TerminalIO::out() << "download-resume <bag>\tResume download of <bag>\n";
    td::TerminalIO::out() << "upload-pause <bag>\tPause upload of <bag>\n";
    td::TerminalIO::out() << "upload-resume <bag>\tResume upload of <bag>\n";
    td::TerminalIO::out() << "priority-all <bag> <p>\tSet priority of all files in <bag> to <p>\n";
    td::TerminalIO::out() << "\tPriority is in [0..255], 0 - don't download\n";
    td::TerminalIO::out() << "priority-idx <bag> <idx> <p>\tSet priority of file #<idx> in <bag> to <p>\n";
    td::TerminalIO::out() << "\tPriority is in [0..255], 0 - don't download\n";
    td::TerminalIO::out() << "priority-name <bag> <name> <p>\tSet priority of file <name> in <bag> to <p>\n";
    td::TerminalIO::out() << "\tPriority is in [0..255], 0 - don't download\n";
    td::TerminalIO::out() << "remove <bag> [--remove-files]\tRemove <bag>\n";
    td::TerminalIO::out() << "\t--remove-files - also remove all files\n";
    td::TerminalIO::out() << "load-from <bag> [--meta meta] [--files path]\tProvide meta and data for an existing "
                             "incomplete bag.\n";
    td::TerminalIO::out() << "\t--meta meta\ttorrent info and header will be inited (if not ready) from meta file\n";
    td::TerminalIO::out() << "\t--files path\tdata for files will be taken from here\n";
    td::TerminalIO::out() << "new-contract-message <bag> <file> [--query-id id] --provider <provider>\tCreate "
                             "\"new contract message\" for storage provider. Saves message body to <file>.\n";
    td::TerminalIO::out() << "\t<provider>\tAddress of storage provider account to take parameters from.\n";
    td::TerminalIO::out() << "new-contract-message <bag> <file> [--query-id id] --rate <rate> --max-span "
                             "<max-span>\tSame thing, but parameters are not fetched automatically.\n";
    td::TerminalIO::out() << "exit\tExit\n";
    td::TerminalIO::out() << "quit\tExit\n";
    td::TerminalIO::out() << "setverbosity <level>\tSet vetbosity to <level> in [0..10]\n";
    td::TerminalIO::out() << "\nStorage provider control:\n";
    td::TerminalIO::out() << "import-pk <file>\tImport private key from <file>\n";
    td::TerminalIO::out() << "deploy-provider\tInit storage provider by deploying a new provider smart contract\n";
    td::TerminalIO::out()
        << "init-provider <smc-addr>\tInit storage provider using the existing provider smart contract\n";
    td::TerminalIO::out() << "remove-storage-provider\tRemove storage provider\n";
    td::TerminalIO::out()
        << "\tSmart contracts in blockchain and bags will remain intact, but they will not be managed anymore\n";
    td::TerminalIO::out() << "get-provider-params [address] [--json]\tPrint parameters of the smart contract\n";
    td::TerminalIO::out()
        << "\taddress\tAddress of a smart contract. Default is the provider managed by this daemon.\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out() << "set-provider-params [--accept x] [--rate x] [--max-span x] [--min-file-size x] "
                             "[--max-file-size x]\tSet parameters of the smart contract\n";
    td::TerminalIO::out() << "\t--accept\tAccept new contracts: 0 (no) or 1 (yes)\n";
    td::TerminalIO::out() << "\t--rate\tPrice of storage, nanoTON per MB*day\n";
    td::TerminalIO::out() << "\t--max-span\n";
    td::TerminalIO::out() << "\t--min-file-size\tMinimal total size of a bag of files (bytes)\n";
    td::TerminalIO::out() << "\t--max-file-size\tMaximal total size of a bag of files (bytes)\n";
    td::TerminalIO::out()
        << "get-provider-info [--balances] [--contracts] [--json]\tPrint information about storage provider\n";
    td::TerminalIO::out() << "\t--contracts\tPrint list of storage contracts\n";
    td::TerminalIO::out() << "\t--balances\tPrint balances of the main contract and storage contracts\n";
    td::TerminalIO::out() << "\t--json\tOutput in json\n";
    td::TerminalIO::out()
        << "set-provider-config [--max-contracts x] [--max-total-size x]\tSet configuration parameters\n";
    td::TerminalIO::out() << "\t--max-contracts\tMaximal number of storage contracts\n";
    td::TerminalIO::out() << "\t--max-total-size\tMaximal total size storage contracts (in bytes)\n";
    td::TerminalIO::out() << "withdraw <address>\tSend bounty from storage contract <address> to the main contract\n";
    td::TerminalIO::out() << "withdraw-all\tSend bounty from all storage contracts (where at least 1 TON is available) "
                             "to the main contract\n";
    td::TerminalIO::out()
        << "send-coins <address> <amount> [--message msg]\tSend <amount> nanoTON to <address> from the main contract\n";
    td::TerminalIO::out()
        << "close-contract <address>\tClose storage contract <address> and delete bag (if possible)\n";
    command_finished(td::Status::OK());
    return td::Status::OK();
  }

  td::Status execute_set_verbosity(int level) {
    auto query = create_tl_object<ton_api::storage_daemon_setVerbosity>(level);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Success\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_create(std::string path, std::string description, bool upload, bool copy, bool json) {
    TRY_RESULT_PREFIX_ASSIGN(path, td::realpath(path), "Invalid path: ");
    auto query = create_tl_object<ton_api::storage_daemon_createTorrent>(path, description, upload, copy);
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 td::TerminalIO::out() << "Bag created\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::print_torrent_full, R.move_as_ok());
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_add_by_hash(td::Bits256 hash, std::string root_dir, bool paused, bool upload,
                                 td::optional<std::vector<std::string>> partial, bool json) {
    if (!root_dir.empty()) {
      TRY_STATUS_PREFIX(td::mkpath(root_dir), "Failed to create directory: ");
      TRY_STATUS_PREFIX(td::mkdir(root_dir), "Failed to create directory: ");
      TRY_RESULT_PREFIX_ASSIGN(root_dir, td::realpath(root_dir), "Invalid path: ");
    }
    std::vector<tl_object_ptr<ton_api::storage_PriorityAction>> priorities;
    if (partial) {
      priorities.push_back(create_tl_object<ton_api::storage_priorityAction_all>(0));
      for (std::string& f : partial.value()) {
        priorities.push_back(create_tl_object<ton_api::storage_priorityAction_name>(std::move(f), 1));
      }
    }
    auto query = create_tl_object<ton_api::storage_daemon_addByHash>(hash, std::move(root_dir), !paused, upload,
                                                                     std::move(priorities));
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 td::TerminalIO::out() << "Bag added\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::print_torrent_full, R.move_as_ok());
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_add_by_meta(std::string meta_file, std::string root_dir, bool paused, bool upload,
                                 td::optional<std::vector<std::string>> partial, bool json) {
    TRY_RESULT_PREFIX(meta, td::read_file(meta_file), "Failed to read meta: ");
    if (!root_dir.empty()) {
      TRY_STATUS_PREFIX(td::mkpath(root_dir), "Failed to create directory: ");
      TRY_STATUS_PREFIX(td::mkdir(root_dir), "Failed to create directory: ");
      TRY_RESULT_PREFIX_ASSIGN(root_dir, td::realpath(root_dir), "Invalid path: ");
    }
    std::vector<tl_object_ptr<ton_api::storage_PriorityAction>> priorities;
    if (partial) {
      priorities.push_back(create_tl_object<ton_api::storage_priorityAction_all>(0));
      for (std::string& f : partial.value()) {
        priorities.push_back(create_tl_object<ton_api::storage_priorityAction_name>(std::move(f), 1));
      }
    }
    auto query = create_tl_object<ton_api::storage_daemon_addByMeta>(std::move(meta), std::move(root_dir), !paused,
                                                                     upload, std::move(priorities));
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 td::TerminalIO::out() << "Bag added\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::print_torrent_full, R.move_as_ok());
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_list(bool with_hashes, bool json) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrents>();
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentList>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::print_torrent_list, R.move_as_ok(), with_hashes);
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_get(td::Bits256 hash, bool json) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrentFull>(hash);
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::print_torrent_full, R.move_as_ok());
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_get_meta(td::Bits256 hash, std::string meta_file) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrentMeta>(hash);
    send_query(std::move(query),
               [SelfId = actor_id(this), meta_file](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentMeta>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 auto data = std::move(R.ok_ref()->meta_);
                 auto S = td::write_file(meta_file, data);
                 if (S.is_error()) {
                   td::actor::send_closure(
                       SelfId, &StorageDaemonCli::command_finished,
                       S.move_as_error_prefix(PSTRING() << "Failed to write meta (" << data.size() << " B): "));
                   return;
                 }
                 td::TerminalIO::out() << "Saved meta (" << data.size() << " B)\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_get_peers(td::Bits256 hash, bool json) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrentPeers>(hash);
    send_query(
        std::move(query), [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_peerList>> R) {
          if (R.is_error()) {
            return;
          }
          if (json) {
            print_json(R.ok());
            td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
            return;
          }
          auto obj = R.move_as_ok();
          td::TerminalIO::out() << "BagID " << hash.to_hex() << "\n";
          td::TerminalIO::out() << "Download speed: " << td::format::as_size((td::uint64)obj->download_speed_)
                                << "/s\n";
          td::TerminalIO::out() << "Upload speed: " << td::format::as_size((td::uint64)obj->upload_speed_) << "/s\n";
          td::TerminalIO::out() << "Peers: " << obj->peers_.size() << "\n";
          std::vector<std::vector<std::string>> table;
          table.push_back({"ADNL id", "Address", "Download", "Upload", "Ready"});
          for (auto& peer : obj->peers_) {
            std::vector<std::string> row;
            row.push_back(PSTRING() << peer->adnl_id_);
            row.push_back(peer->ip_str_);
            row.push_back(PSTRING() << td::format::as_size((td::uint64)peer->download_speed_) << "/s");
            row.push_back(PSTRING() << td::format::as_size((td::uint64)peer->upload_speed_) << "/s");
            if (obj->total_parts_ > 0) {
              char buf[10];
              snprintf(buf, sizeof(buf), "%5.1f%%", (double)peer->ready_parts_ / (double)obj->total_parts_ * 100);
              row.push_back(buf);
            } else {
              row.push_back("???");
            }
            table.push_back(std::move(row));
          }
          print_table(table);
          td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
        });
    return td::Status::OK();
  }

  td::Status execute_set_active_download(td::Bits256 hash, bool active) {
    auto query = create_tl_object<ton_api::storage_daemon_setActiveDownload>(hash, active);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Success\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_active_upload(td::Bits256 hash, bool active) {
    auto query = create_tl_object<ton_api::storage_daemon_setActiveUpload>(hash, active);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Success\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_priority_all(td::Bits256 hash, td::uint8 priority) {
    auto query = create_tl_object<ton_api::storage_daemon_setFilePriorityAll>(hash, priority);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_SetPriorityStatus>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (R.ok()->get_id() == ton_api::storage_daemon_prioritySet::ID) {
                   td::TerminalIO::out() << "Priority was set\n";
                 } else {
                   td::TerminalIO::out() << "Torrent header is not available, priority will be set later\n";
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_priority_idx(td::Bits256 hash, td::uint64 idx, td::uint8 priority) {
    auto query = create_tl_object<ton_api::storage_daemon_setFilePriorityByIdx>(hash, idx, priority);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_SetPriorityStatus>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (R.ok()->get_id() == ton_api::storage_daemon_prioritySet::ID) {
                   td::TerminalIO::out() << "Priority was set\n";
                 } else {
                   td::TerminalIO::out() << "Torrent header is not available, priority will be set later\n";
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_priority_name(td::Bits256 hash, std::string name, td::uint8 priority) {
    auto query = create_tl_object<ton_api::storage_daemon_setFilePriorityByName>(hash, std::move(name), priority);
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_SetPriorityStatus>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (R.ok()->get_id() == ton_api::storage_daemon_prioritySet::ID) {
                   td::TerminalIO::out() << "Priority was set\n";
                 } else {
                   td::TerminalIO::out() << "Torrent header is not available, priority will be set later\n";
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_remove(td::Bits256 hash, bool remove_files) {
    auto query = create_tl_object<ton_api::storage_daemon_removeTorrent>(hash, remove_files);
    send_query(std::move(query),
               [SelfId = actor_id(this), hash](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Success\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::delete_id, hash);
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_load_from(td::Bits256 hash, std::string meta, std::string path) {
    if (meta.empty() && path.empty()) {
      return td::Status::Error("Expected meta or files");
    }
    td::BufferSlice meta_data;
    if (!meta.empty()) {
      TRY_RESULT_PREFIX_ASSIGN(meta_data, td::read_file(meta), "Failed to read meta: ");
    }
    if (!path.empty()) {
      TRY_RESULT_PREFIX_ASSIGN(path, td::realpath(path), "Invalid path: ");
    }
    auto query = create_tl_object<ton_api::storage_daemon_loadFrom>(hash, std::move(meta_data), std::move(path));
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_torrent>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 auto torrent = R.move_as_ok();
                 td::TerminalIO::out() << "Loaded data for bag " << torrent->hash_.to_hex() << "\n";
                 if (torrent->flags_ & 4) {  // fatal error
                   td::TerminalIO::out() << "FATAL ERROR: " << torrent->fatal_error_ << "\n";
                 }
                 if (torrent->flags_ & 1) {  // info ready
                   td::TerminalIO::out() << "Total size: " << td::format::as_size(torrent->total_size_) << "\n";
                   if (torrent->flags_ & 2) {  // header ready
                     td::TerminalIO::out() << "Ready: " << td::format::as_size(torrent->downloaded_size_) << "/"
                                           << td::format::as_size(torrent->included_size_)
                                           << (torrent->completed_ ? " (completed)" : "") << "\n";
                   } else {
                     td::TerminalIO::out() << "Torrent header is not ready\n";
                   }
                 } else {
                   td::TerminalIO::out() << "Torrent info is not ready\n";
                 }
               });
    return td::Status::OK();
  }

  td::Status execute_new_contract_message(td::Bits256 hash, std::string file, td::uint64 query_id,
                                          td::optional<std::string> provider_address, td::optional<std::string> rate,
                                          td::optional<td::uint32> max_span) {
    tl_object_ptr<ton_api::storage_daemon_NewContractParams> params;
    if (provider_address) {
      if (rate || max_span) {
        return td::Status::Error("Incompatible flags");
      }
      params = create_tl_object<ton_api::storage_daemon_newContractParamsAuto>(provider_address.unwrap());
    } else {
      if (!rate || !max_span) {
        return td::Status::Error("No flags are set");
      }
      params = create_tl_object<ton_api::storage_daemon_newContractParams>(rate.unwrap(), max_span.unwrap());
    }
    auto query = create_tl_object<ton_api::storage_daemon_getNewContractMessage>(hash, query_id, std::move(params));
    send_query(std::move(query), [SelfId = actor_id(this),
                                  file](td::Result<tl_object_ptr<ton_api::storage_daemon_newContractMessage>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      auto S = td::write_file(file, obj->body_);
      if (S.is_error()) {
        td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished,
                                S.move_as_error_prefix(PSTRING() << "Failed to write to file: "));
        return;
      }
      td::TerminalIO::out() << "Saved message body to file\n";
      td::TerminalIO::out() << "Rate (nanoTON per mb*day): " << obj->rate_ << "\n";
      td::TerminalIO::out() << "Max span: " << obj->max_span_ << "\n";
      td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
    });
    return td::Status::OK();
  }

  td::Status execute_import_pk(std::string file) {
    TRY_RESULT(data, td::read_file_secure(file));
    TRY_RESULT(pk, ton::PrivateKey::import(data.as_slice()));
    auto query = create_tl_object<ton_api::storage_daemon_importPrivateKey>(pk.tl());
    send_query(
        std::move(query), [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_keyHash>> R) {
          if (R.is_error()) {
            return;
          }
          td::TerminalIO::out() << "Imported private key. Public key hash: " << R.ok()->key_hash_.to_hex() << "\n";
          td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
        });
    return td::Status::OK();
  }

  td::Status execute_deploy_provider() {
    auto query = create_tl_object<ton_api::storage_daemon_deployProvider>();
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_providerAddress>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 auto obj = R.move_as_ok();
                 block::StdAddress std_address;
                 CHECK(std_address.parse_addr(obj->address_));
                 std_address.bounceable = false;
                 td::TerminalIO::out() << "Address: " << obj->address_ << "\n";
                 td::TerminalIO::out() << "Non-bounceable address: " << std_address.rserialize() << "\n";
                 td::TerminalIO::out()
                     << "Send a non-bounceable message with 1 TON to this address to initialize smart contract.\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_init_provider(std::string address) {
    auto query = create_tl_object<ton_api::storage_daemon_initProvider>(std::move(address));
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Address of the storage provider was set\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_remove_storage_provider() {
    auto query = create_tl_object<ton_api::storage_daemon_removeStorageProvider>();
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Storage provider removed\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_get_provider_params(std::string address, bool json) {
    auto query = create_tl_object<ton_api::storage_daemon_getProviderParams>(address);
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_provider_params>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 auto params = R.move_as_ok();
                 td::TerminalIO::out() << "Storage provider parameters:\n";
                 td::TerminalIO::out() << "Accept new contracts: " << params->accept_new_contracts_ << "\n";
                 td::TerminalIO::out() << "Rate (nanoTON per day*MB): " << params->rate_per_mb_day_ << "\n";
                 td::TerminalIO::out() << "Max span: " << (td::uint32)params->max_span_ << "\n";
                 td::TerminalIO::out() << "Min file size: " << (td::uint64)params->minimal_file_size_ << "\n";
                 td::TerminalIO::out() << "Max file size: " << (td::uint64)params->maximal_file_size_ << "\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_provider_params(OptionalProviderParams new_params) {
    auto query_get = create_tl_object<ton_api::storage_daemon_getProviderParams>();
    send_query(std::move(query_get), [SelfId = actor_id(this), new_params = std::move(new_params)](
                                         td::Result<tl_object_ptr<ton_api::storage_daemon_provider_params>> R) mutable {
      if (R.is_error()) {
        return;
      }
      td::actor::send_closure(SelfId, &StorageDaemonCli::execute_set_provider_params_cont, R.move_as_ok(),
                              std::move(new_params));
    });
    return td::Status::OK();
  }

  void execute_set_provider_params_cont(tl_object_ptr<ton_api::storage_daemon_provider_params> params,
                                        OptionalProviderParams new_params) {
    if (new_params.accept_new_contracts) {
      params->accept_new_contracts_ = new_params.accept_new_contracts.unwrap();
    }
    if (new_params.rate_per_mb_day) {
      params->rate_per_mb_day_ = new_params.rate_per_mb_day.unwrap();
    }
    if (new_params.max_span) {
      params->max_span_ = new_params.max_span.unwrap();
    }
    if (new_params.minimal_file_size) {
      params->minimal_file_size_ = new_params.minimal_file_size.unwrap();
    }
    if (new_params.maximal_file_size) {
      params->maximal_file_size_ = new_params.maximal_file_size.unwrap();
    }
    td::TerminalIO::out() << "Sending external message to update provider parameters...\n";
    auto query_set = create_tl_object<ton_api::storage_daemon_setProviderParams>(std::move(params));
    send_query(std::move(query_set),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) mutable {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Storage provider parameters were updated\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
  }

  td::Status execute_get_provider_info(bool with_balances, bool with_contracts, bool json) {
    auto query = create_tl_object<ton_api::storage_daemon_getProviderInfo>(with_balances, with_contracts);
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_providerInfo>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 if (json) {
                   print_json(R.ok());
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                   return;
                 }
                 auto info = R.move_as_ok();
                 td::TerminalIO::out() << "Storage provider " << info->address_ << "\n";
                 td::TerminalIO::out() << "Storage contracts: " << (td::uint32)info->contracts_count_ << " / "
                                       << (td::uint32)info->config_->max_contracts_ << "\n";
                 td::TerminalIO::out() << "Total size: " << size_to_str(info->contracts_total_size_) << " / "
                                       << size_to_str(info->config_->max_total_size_) << "\n";
                 if (with_balances) {
                   td::TerminalIO::out() << "Main contract balance: " << coins_to_str(info->balance_) << " TON\n";
                 }
                 if (with_contracts) {
                   td::TerminalIO::out() << "Storage contracts: " << info->contracts_.size() << "\n";
                   std::vector<std::vector<std::string>> table;
                   table.push_back({"Address", "BagID", "Created at", "Size", "State"});
                   if (with_balances) {
                     table.back().push_back("Client$");
                     table.back().push_back("Contract$");
                   }
                   for (const auto& c : info->contracts_) {
                     table.emplace_back();
                     table.back().push_back(c->address_);
                     table.back().push_back(c->torrent_.to_hex());
                     table.back().push_back(time_to_str(c->created_time_));
                     table.back().push_back(size_to_str(c->file_size_));
                     // enum State { st_downloading = 0, st_downloaded = 1, st_active = 2, st_closing = 3 };
                     switch (c->state_) {
                       case 0:
                         table.back().push_back("Downloading (" + size_to_str(c->downloaded_size_) + ")");
                         break;
                       case 1:
                         table.back().push_back("Downloaded");
                         break;
                       case 2:
                         table.back().push_back("Active");
                         break;
                       case 3:
                         table.back().push_back("Closing");
                         break;
                       default:
                         table.back().push_back("???");
                     }
                     if (with_balances) {
                       table.back().push_back(coins_to_str(c->client_balance_));
                       table.back().push_back(coins_to_str(c->contract_balance_));
                     }
                   }
                   print_table(table);
                 }
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_set_provider_config(OptionalProviderConfig new_config) {
    auto query_get = create_tl_object<ton_api::storage_daemon_getProviderInfo>(false, false);
    send_query(std::move(query_get), [SelfId = actor_id(this), new_config = std::move(new_config)](
                                         td::Result<tl_object_ptr<ton_api::storage_daemon_providerInfo>> R) mutable {
      if (R.is_error()) {
        return;
      }
      auto info = R.move_as_ok();
      td::actor::send_closure(SelfId, &StorageDaemonCli::execute_set_provider_config_cont, std::move(info->config_),
                              std::move(new_config));
    });
    return td::Status::OK();
  }

  void execute_set_provider_config_cont(tl_object_ptr<ton_api::storage_daemon_providerConfig> config,
                                        OptionalProviderConfig new_config) {
    if (new_config.max_contracts) {
      config->max_contracts_ = new_config.max_contracts.unwrap();
    }
    if (new_config.max_total_size) {
      config->max_total_size_ = new_config.max_total_size.unwrap();
    }
    auto query_set = create_tl_object<ton_api::storage_daemon_setProviderConfig>(std::move(config));
    send_query(std::move(query_set),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) mutable {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Storage provider config was updated\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
  }

  td::Status execute_withdraw(std::string address) {
    auto query = create_tl_object<ton_api::storage_daemon_withdraw>(std::move(address));
    td::TerminalIO::out() << "Sending external message...\n";
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Bounty was withdrawn\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_withdraw_all() {
    auto query = create_tl_object<ton_api::storage_daemon_getProviderInfo>(true, true);
    send_query(std::move(query),
               [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_providerInfo>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 auto info = R.move_as_ok();
                 std::vector<std::string> addresses;
                 for (auto& contract : info->contracts_) {
                   if (contract->state_ != 2) {
                     continue;
                   }
                   td::RefInt256 remaining = td::dec_string_to_int256(contract->contract_balance_) -
                                             td::dec_string_to_int256(contract->client_balance_);
                   if (remaining < td::make_refint(1'000'000'000)) {
                     continue;
                   }
                   td::TerminalIO::out() << "Withdrawing from " << contract->address_ << " (" << coins_to_str(remaining)
                                         << " TON)\n";
                   addresses.push_back(contract->address_);
                 }
                 if (addresses.empty()) {
                   td::TerminalIO::out() << "Nothing to withdraw\n";
                   td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
                 } else {
                   td::actor::send_closure(SelfId, &StorageDaemonCli::execute_withdraw_all_cont, std::move(addresses));
                 }
               });
    return td::Status::OK();
  }

  void execute_withdraw_all_cont(std::vector<std::string> addresses) {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    ig.add_promise([SelfId = actor_id(this), cnt = addresses.size()](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, R.move_as_error());
        return;
      }
      td::TerminalIO::out() << "Sent bounty from " << cnt << " contracts\n";
      td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
    });
    for (auto& address : addresses) {
      auto query = create_tl_object<ton_api::storage_daemon_withdraw>(std::move(address));
      send_query(std::move(query),
                 ig.get_promise().wrap([](tl_object_ptr<ton_api::storage_daemon_success>) { return td::Unit(); }));
    }
  }

  td::Status execute_send_coins(std::string address, std::string amount, std::string message) {
    auto query =
        create_tl_object<ton_api::storage_daemon_sendCoins>(std::move(address), std::move(amount), std::move(message));
    td::TerminalIO::out() << "Sending external messages...\n";
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Internal message was sent\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  td::Status execute_close_contract(std::string address) {
    auto query = create_tl_object<ton_api::storage_daemon_closeStorageContract>(std::move(address));
    send_query(std::move(query),
               [SelfId = actor_id(this)](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
                 if (R.is_error()) {
                   return;
                 }
                 td::TerminalIO::out() << "Closing storage contract\n";
                 td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, td::Status::OK());
               });
    return td::Status::OK();
  }

  template <typename T>
  void send_query(tl_object_ptr<T> query, td::Promise<typename T::ReturnType> promise, bool process_error = true) {
    td::actor::send_closure(
        client_, &adnl::AdnlExtClient::send_query, "q", serialize_tl_object(query, true), td::Timestamp::in(1800.0),
        [SelfId = actor_id(this), promise = std::move(promise), process_error](td::Result<td::BufferSlice> R) mutable {
          td::Result<typename T::ReturnType> result;
          if (R.is_error()) {
            if (R.error().message().empty() && R.error().code() == ErrorCode::cancelled) {
              result = td::Status::Error("Query error: failed to connect");
            } else {
              result = R.move_as_error_prefix("Query error: ");
            }
          } else {
            td::BufferSlice data = R.move_as_ok();
            result = fetch_tl_object<typename T::ReturnType::element_type>(data, true);
            if (result.is_error()) {
              auto R3 = fetch_tl_object<ton_api::storage_daemon_queryError>(data, true);
              if (R3.is_ok()) {
                result = td::Status::Error("Query error: " + R3.ok()->message_);
              }
            }
          }
          if (result.is_ok()) {
            promise.set_value(result.move_as_ok());
          } else {
            promise.set_error(result.error().clone());
            if (process_error) {
              td::actor::send_closure(SelfId, &StorageDaemonCli::command_finished, result.move_as_error());
            }
          }
        });
  }

  void command_finished(td::Status S) {
    if (S.is_error()) {
      td::TerminalIO::out() << S.message() << "\n";
      if (batch_mode_) {
        std::exit(2);
      }
    } else if (batch_mode_) {
      if (cur_command_ == commands_.size()) {
        std::exit(0);
      } else {
        parse_line(td::BufferSlice(commands_[cur_command_++]));
      }
    }
  }

 private:
  td::IPAddress server_ip_;
  PrivateKey client_private_key_;
  PublicKey server_public_key_;
  std::vector<std::string> commands_;
  bool batch_mode_ = false;
  bool batch_started_ = false;
  size_t cur_command_ = 0;
  td::actor::ActorOwn<adnl::AdnlExtClient> client_;
  td::actor::ActorOwn<td::TerminalIO> io_;

  std::map<td::uint32, td::Bits256> id_to_hash_;
  std::map<td::Bits256, td::uint32> hash_to_id_;
  td::uint32 cur_id_ = 0;

  void add_id(td::Bits256 hash) {
    if (hash_to_id_.emplace(hash, cur_id_).second) {
      id_to_hash_[cur_id_++] = hash;
    }
  }

  void delete_id(td::Bits256 hash) {
    auto it = hash_to_id_.find(hash);
    if (it != hash_to_id_.end()) {
      id_to_hash_.erase(it->second);
      hash_to_id_.erase(it);
    }
  }

  void update_ids(std::vector<td::Bits256> hashes) {
    for (const td::Bits256& hash : hashes) {
      add_id(hash);
    }
    std::sort(hashes.begin(), hashes.end());
    for (auto it = hash_to_id_.begin(); it != hash_to_id_.end();) {
      if (std::binary_search(hashes.begin(), hashes.end(), it->first)) {
        ++it;
      } else {
        id_to_hash_.erase(it->second);
        it = hash_to_id_.erase(it);
      }
    }
  }

  void print_torrent_full(tl_object_ptr<ton_api::storage_daemon_torrentFull> ptr) {
    auto& obj = *ptr;
    add_id(obj.torrent_->hash_);
    td::TerminalIO::out() << "BagID = " << obj.torrent_->hash_.to_hex() << "\n";
    td::TerminalIO::out() << "Index = " << hash_to_id_[obj.torrent_->hash_] << "\n";
    if (obj.torrent_->flags_ & 4) {  // fatal error
      td::TerminalIO::out() << "FATAL ERROR: " << obj.torrent_->fatal_error_ << "\n";
    }
    if (obj.torrent_->flags_ & 1) {  // info ready
      if (!obj.torrent_->description_.empty()) {
        td::TerminalIO::out() << "-----------------------------------\n";
        td::TerminalIO::out() << obj.torrent_->description_ << "\n";
        td::TerminalIO::out() << "-----------------------------------\n";
      }
      if (obj.torrent_->flags_ & 2) {  // header ready
        td::TerminalIO::out() << "Downloaded: " << td::format::as_size(obj.torrent_->downloaded_size_) << "/"
                              << td::format::as_size(obj.torrent_->included_size_)
                              << (obj.torrent_->completed_
                                      ? " (completed)"
                                      : " (remaining " +
                                            size_to_str(obj.torrent_->included_size_ - obj.torrent_->downloaded_size_) +
                                            ")")
                              << "\n";
        td::TerminalIO::out() << "Dir name: " << obj.torrent_->dir_name_ << "\n";
      }
      td::TerminalIO::out() << "Total size: " << td::format::as_size(obj.torrent_->total_size_) << "\n";
    } else {
      td::TerminalIO::out() << "Torrent info is not available\n";
    }
    if (obj.torrent_->completed_) {
    } else if (obj.torrent_->active_download_) {
      td::TerminalIO::out() << "Download speed: " << td::format::as_size((td::uint64)obj.torrent_->download_speed_)
                            << "/s\n";
    } else {
      td::TerminalIO::out() << "Download paused\n";
    }
    if (obj.torrent_->active_upload_) {
      td::TerminalIO::out() << "Upload speed: " << td::format::as_size((td::uint64)obj.torrent_->upload_speed_)
                            << "/s\n";
    } else {
      td::TerminalIO::out() << "Upload paused\n";
    }
    td::TerminalIO::out() << "Root dir: " << obj.torrent_->root_dir_ << "\n";
    if (obj.torrent_->flags_ & 2) {  // header ready
      td::TerminalIO::out() << obj.files_.size() << " files:\n";
      td::TerminalIO::out() << "######  Prior   Ready/Size       Name\n";
      td::uint32 i = 0;
      for (const auto& f : obj.files_) {
        char str[64];
        char priority[4] = "---";
        if (f->priority_ > 0) {
          CHECK(f->priority_ <= 255);
          snprintf(priority, sizeof(priority), "%03d", f->priority_);
        }
        snprintf(str, sizeof(str), "%6u: (%s) %7s/%-7s %s  ", i, priority,
                 f->priority_ == 0 ? "---" : size_to_str(f->downloaded_size_).c_str(), size_to_str(f->size_).c_str(),
                 ((f->downloaded_size_ == f->size_ && f->priority_ > 0) ? "+" : " "));
        td::TerminalIO::out() << str << f->name_ << "\n";
        ++i;
      }
    } else {
      td::TerminalIO::out() << "Torrent header is not available\n";
    }
  }

  void print_torrent_list(tl_object_ptr<ton_api::storage_daemon_torrentList> ptr, bool with_hashes) {
    auto& obj = *ptr;
    std::vector<td::Bits256> hashes;
    for (const auto& torrent : obj.torrents_) {
      hashes.push_back(torrent->hash_);
    }
    update_ids(std::move(hashes));
    std::sort(obj.torrents_.begin(), obj.torrents_.end(),
              [&](const tl_object_ptr<ton_api::storage_daemon_torrent>& a,
                  const tl_object_ptr<ton_api::storage_daemon_torrent>& b) {
                return hash_to_id_[a->hash_] < hash_to_id_[b->hash_];
              });
    td::TerminalIO::out() << obj.torrents_.size() << " bags\n";
    std::vector<std::vector<std::string>> table;
    table.push_back({"#####", "BagID", "Description", "Downloaded", "Total", "Download", "Upload"});
    for (const auto& torrent : obj.torrents_) {
      std::vector<std::string> row;
      row.push_back(std::to_string(hash_to_id_[torrent->hash_]));
      std::string hash_str = torrent->hash_.to_hex();
      if (!with_hashes) {
        hash_str = hash_str.substr(0, 8) + "...";
      }
      row.push_back(hash_str);
      std::string description = torrent->description_;
      for (size_t i = 0; i < description.size(); ++i) {
        if (!is_whitespace(description[i])) {
          description.erase(description.begin(), description.begin() + i);
          break;
        }
      }
      for (size_t i = 0; i < description.size(); ++i) {
        if (description[i] == '\n') {
          description.resize(i);
          break;
        }
      }
      if (description.size() > 45) {
        description.resize(42);
        description += "...";
      }
      row.push_back(description);
      bool info_ready = torrent->flags_ & 1;
      bool header_ready = torrent->flags_ & 2;
      std::string downloaded_size = size_to_str(torrent->downloaded_size_);
      std::string included_size = header_ready ? size_to_str(torrent->included_size_) : "???";
      std::string total_size = info_ready ? size_to_str(torrent->total_size_) : "???";
      std::string status;
      if (torrent->flags_ & 4) {  // fatal error
        status = "FATAL ERROR: " + torrent->fatal_error_;
      } else {
        status =
            torrent->completed_
                ? "COMPLETED"
                : (torrent->active_download_ ? size_to_str((td::uint64)torrent->download_speed_) + "/s" : "Paused");
      }
      row.push_back(downloaded_size.append("/").append(included_size));
      row.push_back(total_size);
      row.push_back(status);
      row.push_back(torrent->active_upload_ ? size_to_str((td::uint64)torrent->upload_speed_) + "/s" : "Paused");
      table.push_back(std::move(row));
    }
    print_table(table, {2});
  }
};

}  // namespace ton

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();
  td::IPAddress ip_addr;
  ton::PrivateKey client_private_key;
  ton::PublicKey server_public_key;
  std::vector<std::string> commands;
  td::OptionParser p;
  p.set_description("command-line interface for storage-daemon");
  p.add_option('h', "help", "prints_help", [&]() {
    std::cout << (PSLICE() << p).c_str();
    std::exit(2);
  });
  p.add_checked_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    auto verbosity = td::to_integer<int>(arg);
    SET_VERBOSITY_LEVEL(VERBOSITY_NAME(FATAL) + verbosity);
    return (verbosity >= 0 && verbosity <= 20) ? td::Status::OK() : td::Status::Error("verbosity must be 0..20");
  });
  p.add_option('V', "version", "shows storage-daemon-cli build information", [&]() {
    std::cout << "storage-daemon-cli build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_checked_option('I', "ip", "set ip:port of storage-daemon", [&](td::Slice arg) {
    TRY_STATUS(ip_addr.init_host_port(arg.str()));
    return td::Status::OK();
  });
  p.add_option('c', "cmd", "execute command", [&](td::Slice arg) { commands.push_back(arg.str()); });
  p.add_checked_option('k', "key", "private key", [&](td::Slice arg) {
    TRY_RESULT_PREFIX(data, td::read_file(arg.str()), "failed to read: ");
    TRY_RESULT_ASSIGN(client_private_key, ton::PrivateKey::import(data));
    return td::Status::OK();
  });
  p.add_checked_option('p', "pub", "server public key", [&](td::Slice arg) {
    TRY_RESULT_PREFIX(data, td::read_file(arg.str()), "failed to read: ");
    TRY_RESULT_ASSIGN(server_public_key, ton::PublicKey::import(data));
    return td::Status::OK();
  });

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }
  LOG_IF(FATAL, client_private_key.empty()) << "Client private key is not set";
  LOG_IF(FATAL, server_public_key.empty()) << "Server public key is not set";

  td::actor::Scheduler scheduler({0});
  scheduler.run_in_context([&] {
    td::actor::create_actor<ton::StorageDaemonCli>("console", ip_addr, client_private_key, server_public_key,
                                                   std::move(commands))
        .release();
  });
  scheduler.run();
  return 0;
}
