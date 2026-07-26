/*
 * Copyright (c) 2024-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#pragma once

#include "auto/tl/ton_api.h"
#include "td/utils/Status.h"

namespace ton::validator::consensus {

namespace tl {

using payload = ton_api::validatorSession_candidate;

}

td::Result<td::BufferSlice> serialize_payload(const tl_object_ptr<tl::payload>& payload);
td::Result<tl_object_ptr<tl::payload>> deserialize_payload(td::Slice data, int max_decompressed_data_size);

}  // namespace ton::validator::consensus
