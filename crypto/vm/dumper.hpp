#pragma once
#include "vm/stack.hpp"

namespace vm {
using td::Ref;

class VmDumper {
 public:
  bool enable;
  std::vector<std::vector<StackEntry>>* stacks{};
  std::vector<std::string>* vm_ops{};

  VmDumper(bool enable_, std::vector<std::vector<StackEntry>>* stacks_, std::vector<std::string>* vm_ops_) {
    enable = enable_;
    stacks = stacks_;
    vm_ops = vm_ops_;
  }

  explicit VmDumper(VmDumper *dumper_) {
    stacks = dumper_->stacks;
    vm_ops = dumper_->vm_ops;
    enable = true;
  }

  VmDumper() {
    enable = false;
  }

  void dump_stack(const Ref<vm::Stack>& stack) const {
    std::cerr << "I'm here" << std::endl;

    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    std::vector<StackEntry> current_stack;

    stack->for_each_scalar([&current_stack](const StackEntry& stackPointer) {
      std::cerr << "Got: ";
      stackPointer.dump(std::cerr);
      std::cerr << std::endl;

      current_stack.push_back(stackPointer);
    });

    std::cerr << "Final round";
    stacks->push_back(std::move(current_stack));

    std::cerr << "Done round";
  };

  void dump_op(std::string op) const {
    if (!enable) {
      throw std::invalid_argument("Must be enabled to dump");
    }

    vm_ops->push_back(std::move(op));
  };
};

}  // namespace vm
