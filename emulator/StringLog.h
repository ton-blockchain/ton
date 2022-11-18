#ifndef TON_STRINGLOG_H
#define TON_STRINGLOG_H

#include "td/utils/logging.h"
#include <thread>

class StringLog : public td::LogInterface {
 public:
  StringLog() {
  }

  void append(td::CSlice new_slice, int log_level) override {
    lock.lock();
    str.append(new_slice.str());
    lock.unlock();
  }

  void rotate() override {
  }

  void clear() {
    str.clear();
  }

  std::string get_string() const {
    return str;
  }

 private:
  std::string str;
  std::mutex lock;
};

#endif  //TON_STRINGLOG_H