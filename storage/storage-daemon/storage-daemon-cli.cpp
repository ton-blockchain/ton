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
#include "common/checksum.h"

using namespace ton;

td::Result<std::vector<std::string>> tokenize(td::Slice s) {
  const char* ptr = s.begin();
  auto is_ws = [&](char c) { return strchr(" \t\n\r", c) != nullptr; };
  auto skip_ws = [&]() {
    while (ptr != s.end() && is_ws(*ptr)) {
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
      } else if (*ptr == quote || (!quote && is_ws(*ptr))) {
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

void print_torrent_full(const ton_api::storage_daemon_torrentFull& obj) {
  td::TerminalIO::out() << "Hash = " << obj.torrent_->hash_.to_hex() << "\n";
  if (obj.torrent_->info_ready_) {
    td::TerminalIO::out() << "Size: " << td::format::as_size(obj.torrent_->total_size_) << "\n";
    td::TerminalIO::out() << "Downloaded: " << obj.torrent_->downloaded_frac_ * 100.0 << "%"
                          << (obj.torrent_->completed_ ? " (completed)" : "") << "\n";
    if (!obj.torrent_->description_.empty()) {
      td::TerminalIO::out() << "------------\n";
      td::TerminalIO::out() << obj.torrent_->description_ << "\n";
      td::TerminalIO::out() << "------------\n";
    }
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
  td::TerminalIO::out() << "Root dir: " << obj.torrent_->root_dir_ << "\n";
  if (obj.torrent_->header_ready_) {
    td::TerminalIO::out() << "Files:\n";
    for (const auto& f : obj.files_) {
      char str[64];
      snprintf(str, sizeof(str), "  %8s  %5.1f%%  ", size_to_str(f->size_).c_str(), f->downloaded_frac_ * 100.0);
      td::TerminalIO::out() << str << f->name_ << "\n";
    }
  } else {
    td::TerminalIO::out() << "Torrent header is not available\n";
  }
}

void print_torrent_list(const ton_api::storage_daemon_torrentList& obj) {
  td::TerminalIO::out() << obj.torrents_.size() << " torrents\n";
  for (const auto& torrent : obj.torrents_) {
    char str[256];
    std::string hash = torrent->hash_.to_hex();
    std::string size = torrent->info_ready_ ? size_to_str(torrent->total_size_) : "???";
    double downloaded = torrent->downloaded_frac_ * 100.0;
    std::string speed =
        torrent->completed_
            ? "COMPLETED"
            : (torrent->active_download_ ? size_to_str((td::uint64)torrent->download_speed_) + "/s" : "Paused");
    snprintf(str, sizeof(str), "%64s %7s %5.1f%% %9s", hash.c_str(), size.c_str(), downloaded, speed.c_str());
    td::TerminalIO::out() << str << "\n";
  }
}

class StorageDaemonCli : public td::actor::Actor {
 public:
  explicit StorageDaemonCli(td::IPAddress server_ip) : server_ip_(server_ip) {
  }

  void start_up() override {
    class ExtClientCallback : public adnl::AdnlExtClient::Callback {
     public:
      void on_ready() override {
        LOG(INFO) << "Connected";
      }
      void on_stop_ready() override {
        LOG(WARNING) << "Connection closed";
      }
    };
    CHECK(server_ip_.is_valid());
    auto pk = PrivateKey{privkeys::Ed25519(td::sha256_bits256("storage-daemon-control"))};
    client_ = adnl::AdnlExtClient::create(adnl::AdnlNodeIdFull{pk.compute_public_key()}, server_ip_,
                                          std::make_unique<ExtClientCallback>());

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

  void parse_line(td::BufferSlice line) {
    auto T = tokenize(line);
    if (T.is_error()) {
      td::TerminalIO::out() << "Error: " << T.error().message() << "\n";
      return;
    }
    auto tokens = T.move_as_ok();
    if (tokens.empty()) {
      return;
    }
    if (tokens[0] == "quit" || tokens[0] == "exit") {
      if (tokens.size() != 1) {
        td::TerminalIO::out() << "Unexpected tokens\n";
        return;
      }
      std::_Exit(0);
    } else if (tokens[0] == "help") {
      if (tokens.size() != 1) {
        td::TerminalIO::out() << "Unexpected tokens\n";
        return;
      }
      execute_help();
    } else if (tokens[0] == "setverbosity") {
      if (tokens.size() != 2) {
        td::TerminalIO::out() << "Expected level\n";
        return;
      }
      auto level = td::to_integer_safe<int>(tokens[1]);
      if (level.is_error()) {
        td::TerminalIO::out() << "Error: " << level.move_as_error() << "\n";
        return;
      }
      execute_set_verbosity(level.move_as_ok());
    } else if (tokens[0] == "create") {
      std::string path;
      bool found_path = false;
      std::string description;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "-d") {
            ++i;
            if (i == tokens.size()) {
              td::TerminalIO::out() << "Expected token\n";
              return;
            }
            description = tokens[i];
            continue;
          }
          td::TerminalIO::out() << "Unknown flag " << tokens[i] << "\n";
          return;
        }
        if (found_path) {
          td::TerminalIO::out() << "Unexpected token\n";
          return;
        }
        path = tokens[i];
        found_path = true;
      }
      if (!found_path) {
        td::TerminalIO::out() << "Expected path\n";
        return;
      }
      execute_create(std::move(path), std::move(description));
    } else if (tokens[0] == "add-by-hash") {
      td::Bits256 hash;
      bool found_hash = false;
      std::string root_dir;
      bool start_download = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "-d") {
            ++i;
            if (i == tokens.size()) {
              td::TerminalIO::out() << "Expected token\n";
              return;
            }
            root_dir = tokens[i];
            continue;
          }
          if (tokens[i] == "--download") {
            start_download = true;
            continue;
          }
          td::TerminalIO::out() << "Unknown flag " << tokens[i] << "\n";
          return;
        }
        if (found_hash) {
          td::TerminalIO::out() << "Unexpected token\n";
          return;
        }
        if (hash.from_hex(tokens[i]) != 256) {
          td::TerminalIO::out() << "Invalid hash\n";
          return;
        }
        found_hash = true;
      }
      if (!found_hash) {
        td::TerminalIO::out() << "Expected hash\n";
        return;
      }
      execute_add_by_hash(hash, std::move(root_dir), start_download);
    } else if (tokens[0] == "add-by-meta") {
      std::string meta_file;
      std::string root_dir;
      bool start_download = false;
      for (size_t i = 1; i < tokens.size(); ++i) {
        if (!tokens[i].empty() && tokens[i][0] == '-') {
          if (tokens[i] == "-d") {
            ++i;
            if (i == tokens.size()) {
              td::TerminalIO::out() << "Expected token\n";
              return;
            }
            root_dir = tokens[i];
            continue;
          }
          if (tokens[i] == "--download") {
            start_download = true;
            continue;
          }
          td::TerminalIO::out() << "Unknown flag " << tokens[i] << "\n";
          return;
        }
        if (!meta_file.empty()) {
          td::TerminalIO::out() << "Unexpected token\n";
          return;
        }
        meta_file = tokens[i];
      }
      if (meta_file.empty()) {
        td::TerminalIO::out() << "Expected filename\n";
        return;
      }
      execute_add_by_meta(std::move(meta_file), std::move(root_dir), start_download);
    } else if (tokens[0] == "list") {
      if (tokens.size() != 1) {
        td::TerminalIO::out() << "Unexpected tokens\n";
        return;
      }
      execute_list();
    } else if (tokens[0] == "get") {
      if (tokens.size() != 2) {
        td::TerminalIO::out() << "Expected hash\n";
        return;
      }
      td::Bits256 hash;
      if (hash.from_hex(tokens[1]) != 256) {
        td::TerminalIO::out() << "Invalid hash\n";
        return;
      }
      execute_get(hash);
    } else if (tokens[0] == "get-meta") {
      if (tokens.size() != 3) {
        td::TerminalIO::out() << "Expected hash and file\n";
        return;
      }
      td::Bits256 hash;
      if (hash.from_hex(tokens[1]) != 256) {
        td::TerminalIO::out() << "Invalid hash\n";
        return;
      }
      execute_get_meta(hash, tokens[2]);
    } else if (tokens[0] == "download-pause" || tokens[0] == "download-resume") {
      if (tokens.size() != 2) {
        td::TerminalIO::out() << "Expected hash\n";
        return;
      }
      td::Bits256 hash;
      if (hash.from_hex(tokens[1]) != 256) {
        td::TerminalIO::out() << "Invalid hash\n";
        return;
      }
      execute_set_active_download(hash, tokens[0] == "download-resume");
    } else {
      td::TerminalIO::out() << "Error: unknown command " << tokens[0] << "\n";
    }
  }

  void execute_help() {
    td::TerminalIO::out() << "help\tPrint this help\n";
    td::TerminalIO::out() << "create [-d description] <file/dir>\tCreate torrent from <file/dir>\n";
    td::TerminalIO::out() << "add-by-hash [-d root_dir] [--download] <hash>\tAdd torrent with given <hash> (in hex)\n";
    td::TerminalIO::out() << "\tTorrent will be downloaded to root_dir, "
                             "default is an internal directory of storage-daemon\n";
    td::TerminalIO::out() << "add-by-meta [-d root_dir] [--download] <meta>\tLoad meta from file and add torrent\n";
    td::TerminalIO::out() << "\tTorrent will be downloaded to root_dir, "
                             "default is an internal directory of storage-daemon\n";
    td::TerminalIO::out() << "\t--download - start download immediately\n";
    td::TerminalIO::out() << "list\tPrint list of torrents\n";
    td::TerminalIO::out() << "get <hash>\tPrint information about torrent <hash> (hash in hex)\n";
    td::TerminalIO::out() << "get-meta <hash> <file>\tSave torrent meta of <hash> to <file>\n";
    td::TerminalIO::out() << "download-pause <hash>\tPause download of torrent <hash> (hash in hex)\n";
    td::TerminalIO::out() << "download-resume <hash>\tResume download of torrent <hash> (hash in hex)\n";
    td::TerminalIO::out() << "exit\tExit\n";
    td::TerminalIO::out() << "quit\tExit\n";
    td::TerminalIO::out() << "setverbosity <level>\tSet vetbosity to <level> in [0..10]\n";
  }

  void execute_set_verbosity(int level) {
    auto query = create_tl_object<ton_api::storage_daemon_setVerbosity>(level);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
      if (R.is_error()) {
        return;
      }
      td::TerminalIO::out() << "Success";
    });
  }

  void execute_create(std::string path, std::string description) {
    auto R = td::realpath(path);
    if (R.is_error()) {
      td::TerminalIO::out() << "Invalid path: " << R.move_as_error() << "\n";
      return;
    }
    path = R.move_as_ok();
    auto query = create_tl_object<ton_api::storage_daemon_createTorrent>(path, description);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      td::TerminalIO::out() << "Torrent created\n";
      print_torrent_full(*obj);
    });
  }

  void execute_add_by_hash(td::Bits256 hash, std::string root_dir, bool start_download) {
    if (!root_dir.empty()) {
      auto R = td::realpath(root_dir);
      if (R.is_error()) {
        td::TerminalIO::out() << "Invalid path: " << R.move_as_error() << "\n";
        return;
      }
      root_dir = R.move_as_ok();
    }
    auto query = create_tl_object<ton_api::storage_daemon_addByHash>(hash, root_dir, start_download);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      td::TerminalIO::out() << "Torrent added\n";
      print_torrent_full(*obj);
    });
  }

  void execute_add_by_meta(std::string meta_file, std::string root_dir, bool start_download) {
    auto r_meta = td::read_file(meta_file);
    if (r_meta.is_error()) {
      td::TerminalIO::out() << "Failed to read meta: " << r_meta.move_as_error() << "\n";
      return;
    }
    if (!root_dir.empty()) {
      auto R = td::realpath(root_dir);
      if (R.is_error()) {
        td::TerminalIO::out() << "Invalid path: " << R.move_as_error() << "\n";
        return;
      }
      root_dir = R.move_as_ok();
    }
    auto query = create_tl_object<ton_api::storage_daemon_addByMeta>(r_meta.move_as_ok(), root_dir, start_download);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      td::TerminalIO::out() << "Torrent added\n";
      print_torrent_full(*obj);
    });
  }

  void execute_list() {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrents>();
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentList>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      print_torrent_list(*obj);
    });
  }

  void execute_get(td::Bits256 hash) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrentFull>(hash);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentFull>> R) {
      if (R.is_error()) {
        return;
      }
      auto obj = R.move_as_ok();
      print_torrent_full(*obj);
    });
  }

  void execute_get_meta(td::Bits256 hash, std::string meta_file) {
    auto query = create_tl_object<ton_api::storage_daemon_getTorrentMeta>(hash);
    send_query(std::move(query), [meta_file](td::Result<tl_object_ptr<ton_api::storage_daemon_torrentMeta>> R) {
      if (R.is_error()) {
        return;
      }
      auto data = std::move(R.ok_ref()->meta_);
      auto S = td::write_file(meta_file, data);
      if (S.is_error()) {
        td::TerminalIO::out() << "Failed to write torrent meta (" << data.size() << " B): " << S << "\n";
        return;
      }
      td::TerminalIO::out() << "Saved torrent meta (" << data.size() << " B)\n";
    });
  }

  void execute_set_active_download(td::Bits256 hash, bool active) {
    auto query = create_tl_object<ton_api::storage_daemon_setActiveDownload>(hash, active);
    send_query(std::move(query), [](td::Result<tl_object_ptr<ton_api::storage_daemon_success>> R) {
      if (R.is_error()) {
        return;
      }
      td::TerminalIO::out() << "Success\n";
    });
  }

  template <typename T>
  void send_query(tl_object_ptr<T> query, td::Promise<typename T::ReturnType> promise) {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, "q", serialize_tl_object(query, true),
                            td::Timestamp::in(20.0),
                            [promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
                              if (R.is_error()) {
                                td::TerminalIO::out() << "Query error: " << R.move_as_error() << "\n";
                                promise.set_error(R.move_as_error());
                                return;
                              }
                              td::BufferSlice data = R.move_as_ok();
                              auto R2 = fetch_tl_object<typename T::ReturnType::element_type>(data, true);
                              if (R2.is_error()) {
                                auto R3 = fetch_tl_object<ton_api::storage_daemon_queryError>(data, true);
                                if (R3.is_ok()) {
                                  td::TerminalIO::out() << "Query error: " << R3.move_as_ok()->message_ << "\n";
                                } else {
                                  td::TerminalIO::out() << "Query error: failed to parse answer\n";
                                }
                                promise.set_error(td::Status::Error());
                                return;
                              }
                              promise.set_value(R2.move_as_ok());
                            });
  }

 private:
  td::IPAddress server_ip_;
  td::actor::ActorOwn<adnl::AdnlExtClient> client_;
  td::actor::ActorOwn<td::TerminalIO> io_;
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler();
  td::IPAddress ip_addr;
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

  auto S = p.run(argc, argv);
  if (S.is_error()) {
    std::cerr << S.move_as_error().message().str() << std::endl;
    std::_Exit(2);
  }

  td::actor::Scheduler scheduler({0});
  scheduler.run_in_context([&] { td::actor::create_actor<StorageDaemonCli>("console", ip_addr).release(); });
  scheduler.run();
  return 0;
}
