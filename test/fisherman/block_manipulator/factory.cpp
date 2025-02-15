#include "factory.hpp"

#include "header_corrupter.hpp"
#include "transaction_corrupter.hpp"

namespace test::fisherman {

auto ManipulatorFactory::create(td::JsonValue jv) -> std::shared_ptr<BaseManipulator> {
  auto res = createImpl(std::move(jv));
  if (res.is_error()) {
    throw std::runtime_error("Error while creating manipulator: " + res.error().message().str());
  }
  return res.move_as_ok();
}

auto ManipulatorFactory::createImpl(td::JsonValue jv) -> td::Result<std::shared_ptr<BaseManipulator>> {
  CHECK(jv.type() == td::JsonValue::Type::Object);

  auto &obj = jv.get_object();
  TRY_RESULT(type, td::get_json_object_string_field(obj, "type", false));
  TRY_RESULT(json_config, td::get_json_object_field(obj, "config", td::JsonValue::Type::Object, false));

  if (type == "HeaderCorrupter") {
    return std::make_shared<HeaderCorrupter>(HeaderCorrupter::Config::fromJson(std::move(json_config)));
  }
  if (type == "TransactionCorrupter") {
    return std::make_shared<TransactionCorrupter>(TransactionCorrupter::Config::fromJson(std::move(json_config)));
  }
  return td::Status::Error(400, PSLICE() << "Unknown manipulator type: " << type);
}

}  // namespace test::fisherman
