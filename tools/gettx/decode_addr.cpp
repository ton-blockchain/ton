#include "crypto/block/block-parse.h"
#include <iostream>
#include <string>

int main() {
  std::string friendly_addr = "Ef80UXx731GHxVr0-LYf3DIViMerdo3uJLAG3ykQZFjXz2kW";

  ton::WorkchainId wc;
  ton::StdSmcAddress addr;
  bool bounceable;

  bool success = block::parse_std_account_addr(
    friendly_addr, wc, addr, &bounceable);

  if (success) {
    std::cout << "Workchain: " << wc << std::endl;
    std::cout << "Address (hex): " << addr.to_hex() << std::endl;
  } else {
    std::cerr << "Failed to parse address" << std::endl;
    return 1;
  }

  return 0;
}
