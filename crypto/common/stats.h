/*
 * Copyright (c) 2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include <memory>
#include <optional>

#include "auto/tl/ton_api.h"

namespace ton::stats {

class Tag {
 public:
  virtual ~Tag();

  virtual std::string_view name() const = 0;
};

class Recorder;

class Callback {
 public:
  virtual ~Callback();

  virtual std::unique_ptr<Recorder> get_recorder(const Tag& tag) = 0;
};

class Recorder {
 public:
  virtual ~Recorder();

  virtual void add(const tl_object_ptr<ton_api::Object>& data) = 0;
};

void install_callback(std::unique_ptr<Callback> callback);
std::unique_ptr<Recorder> recorder_for(const Tag& tag);

}  // namespace ton::stats
