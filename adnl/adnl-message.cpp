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

    Copyright 2017-2020 Telegram Systems LLP
*/
#include "adnl/adnl-message.h"
#include "auto/tl/ton_api.hpp"
#include "td/utils/overloaded.h"

namespace ton {

namespace adnl {

AdnlMessage::AdnlMessage(tl_object_ptr<ton_api::adnl_Message> message) {
  ton_api::downcast_call(
      *message.get(),
      td::overloaded(
          [&](ton_api::adnl_message_createChannel &msg) {
            message_ = adnlmessage::AdnlMessageCreateChannel{msg.key_, msg.date_};
          },
          [&](ton_api::adnl_message_confirmChannel &msg) {
            message_ = adnlmessage::AdnlMessageConfirmChannel{msg.key_, msg.peer_key_, msg.date_};
          },
          [&](ton_api::adnl_message_custom &msg) { message_ = adnlmessage::AdnlMessageCustom{std::move(msg.data_)}; },
          [&](ton_api::adnl_message_nop &msg) { message_ = adnlmessage::AdnlMessageNop{}; },
          [&](ton_api::adnl_message_reinit &msg) { message_ = adnlmessage::AdnlMessageReinit{msg.date_}; },
          [&](ton_api::adnl_message_query &msg) {
            message_ = adnlmessage::AdnlMessageQuery{msg.query_id_, std::move(msg.query_)};
          },
          [&](ton_api::adnl_message_answer &msg) {
            message_ = adnlmessage::AdnlMessageAnswer{msg.query_id_, std::move(msg.answer_)};
          },
          [&](ton_api::adnl_message_part &msg) {
            message_ = adnlmessage::AdnlMessagePart{msg.hash_, static_cast<td::uint32>(msg.total_size_),
                                                    static_cast<td::uint32>(msg.offset_), std::move(msg.data_)};
          }));
}

}  // namespace adnl

}  // namespace ton
