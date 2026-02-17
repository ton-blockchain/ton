#pragma once

#include <type_traits>
#include <utility>

namespace td::actor {

template <class T>
struct Ref {
  Ref() = default;

  static Ref adopt(T* p) {
    Ref r;
    r.ptr_ = p;
    return r;
  }

  static Ref share(T* p) {
    if (p)
      p->add_ref();
    Ref r;
    r.ptr_ = p;
    return r;
  }

  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;

  Ref(Ref&& o) noexcept : ptr_(std::exchange(o.ptr_, nullptr)) {
  }
  Ref& operator=(Ref&& o) noexcept {
    if (this != &o) {
      reset();
      ptr_ = std::exchange(o.ptr_, nullptr);
    }
    return *this;
  }

  template <class U>
    requires(std::is_convertible_v<U*, T*> && !std::is_same_v<U, T>)
  Ref(Ref<U>&& o) noexcept : ptr_(std::exchange(o.ptr_, nullptr)) {
  }

  ~Ref() {
    reset();
  }

  void reset() {
    if (ptr_)
      ptr_->dec_ref();
    ptr_ = nullptr;
  }

  Ref share() const {
    return Ref::share(ptr_);
  }

  T* get() const {
    return ptr_;
  }
  T& operator*() const {
    return *ptr_;
  }
  T* operator->() const {
    return ptr_;
  }
  explicit operator bool() const {
    return ptr_ != nullptr;
  }
  T* release() {
    return std::exchange(ptr_, nullptr);
  }

 private:
  template <class U>
  friend struct Ref;
  T* ptr_{nullptr};
};

template <class T, class... Args>
Ref<T> make_ref(Args&&... args) {
  return Ref<T>::adopt(new T(std::forward<Args>(args)...));
}

}  // namespace td::actor
