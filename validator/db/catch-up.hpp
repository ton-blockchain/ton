#pragma once

namespace ton {
namespace validator {

enum class CatchUpMode {
  Force,
  Throttled,
  None,
};

}  // namespace validator
}  // namespace ton
