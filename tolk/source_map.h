#pragma once
#include "tolk.h"

namespace tolk {

struct SourceMapLocation {
  std::string file;
  int offset{};
  int end_offset{};
  long line{};
  long col{};
  long end_line{};
  long end_col{};
  long length{};
};

struct SourceMapVariable {
  /**
   * All information about variable.
   */
  TmpVar data;

  /**
   * If a variable has a constant value (rarely) it will be placed here.
   */
  std::string constant_value;
};

struct SourceMapEntry {
  /**
   * Unique ID of this entry.
   */
  size_t idx{};

  /**
   * If true, entry represents code before first statement.
   */
  bool is_entry{};
  /**
   * If true, entry represents code after last statement.
   */
  bool is_leave{};
  bool is_after_function_call{};
  std::string entry_or_leave_name{};

  /**
   * If true, entry represent assert throw call.
   */
  bool is_assert_throw{};

  /**
   * Human-readable description of current entry.
   */
  std::string descr{};

  /**
   * Location of this entry.
   */
  SourceMapLocation loc{};

  /**
   * Variables available in current position.
   */
  std::vector<SourceMapVariable> vars;

  /**
   * Name oj outer function which contains this code.
   */
  std::string func_name;

  /**
   * If a function was inlined, this field will contain the name
   * of the function where the code was inlined.
   */
  std::string inlined_to_func_name;

  /**
   * Whenever outer function is inlined and how.
   */
  FunctionInlineMode func_inline_mode;

  /**
   * Marks the first instruction of inlined function.
   */
  bool before_inlined_function_call{false};

  /**
   * Marks the last instruction of inlined function.
   */
  bool after_inlined_function_call{false};

  /**
   * The AST node for which this entry was generated.
   */
  std::string ast_kind;

#ifdef TOLK_DEBUG
  /**
   * String representation of `Op` for which this entry was generated.
   */
  std::string opcode;
#endif
};

struct SourceMapGlobalVariable {
  /**
   * Name of this global variable.
   */
  std::string name;
  /**
   * Human-readable type pf this global variable.
   */
  std::string type;
};
}