/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "td/utils/logging.h"

#include "stats.h"

namespace ton::stats {

namespace {

class NullRecorder : public Recorder {
  void add(const tl_object_ptr<ton_api::Object>& data) override {
  }
};

std::unique_ptr<Callback> s_callback;
bool s_logged_no_callback = false;

}  // namespace

Tag::~Tag() = default;
Callback::~Callback() = default;
Recorder::~Recorder() = default;

void install_callback(std::unique_ptr<Callback> callback) {
  s_callback = std::move(callback);
}

std::unique_ptr<Recorder> recorder_for(const Tag& tag) {
  if (!s_callback) {
    if (!s_logged_no_callback) {
      LOG(WARNING) << "Stats recorder is requested before callback is installed";
      s_logged_no_callback = true;
    }

    return std::make_unique<NullRecorder>();
  }

  return s_callback->get_recorder(tag);
}

}  // namespace ton::stats
