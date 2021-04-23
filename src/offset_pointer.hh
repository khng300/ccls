#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>

namespace ccls {
namespace db {
namespace impl {
template <typename T> struct offsetPointer {
private:
  struct nat {};

public:
  typedef T element_type;
  typedef typename std::remove_cv<T>::type value_type;
  typedef T *pointer;
  typedef std::add_lvalue_reference_t<
      std::conditional_t<std::is_void_v<element_type>, nat, element_type>>
      reference;
  typedef std::random_access_iterator_tag iterator_category;
  typedef ptrdiff_t difference_type;

  offsetPointer() noexcept = default;
  offsetPointer(const offsetPointer &p) noexcept { copyAssign(p); }
  template <typename T2>
  offsetPointer(const offsetPointer<T2> &p,
                std::enable_if_t<std::is_convertible_v<T2 *, pointer>,
                                 pointer> = nullptr) noexcept {
    assignRawPtr(p.rawPtr());
  }
  template <typename T2>
  explicit offsetPointer(const offsetPointer<T2> &p,
                         std::enable_if_t<!std::is_convertible_v<T2 *, pointer>,
                                          pointer> = nullptr) noexcept {
    assignRawPtr(p.rawPtr());
  }
  offsetPointer(pointer p) noexcept { assignRawPtr(p); }
  template <typename T2>
  offsetPointer(T2 *p, std::enable_if_t<std::is_convertible_v<T2 *, pointer>,
                                        pointer> = nullptr) noexcept {
    assignRawPtr(p);
  }
  template <typename T2>
  explicit offsetPointer(
      T2 *p, std::enable_if_t<!std::is_convertible_v<T2 *, pointer>, pointer> =
                 nullptr) noexcept {
    assignRawPtr(p);
  }

  offsetPointer &operator=(const offsetPointer &rhs) noexcept {
    copyAssign(rhs);
    return *this;
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  offsetPointer &operator=(const offsetPointer<T2> &rhs) noexcept {
    assignRawPtr(rhs.rawPtr());
    return *this;
  }
  offsetPointer &operator=(pointer rhs) noexcept {
    assignRawPtr(rhs);
    return *this;
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  offsetPointer &operator=(pointer rhs) noexcept {
    assignRawPtr(rhs);
  }

  reference operator*() const noexcept { return *rawPtr(); }

  pointer operator->() const noexcept { return rawPtr(); }

  reference operator[](difference_type idx) const noexcept {
    return rawPtr()[idx];
  }

  offsetPointer &operator+=(difference_type diff) noexcept {
    off += step(diff);
    return *this;
  }
  offsetPointer &operator-=(difference_type diff) noexcept {
    off -= step(diff);
    return *this;
  }

  offsetPointer &operator++() noexcept {
    operator+=(1);
    return *this;
  }
  offsetPointer operator++(int) const noexcept {
    offsetPointer t(*this);
    t += 1;
    return *this;
  }

  offsetPointer &operator--() noexcept {
    operator-=(1);
    return *this;
  }
  offsetPointer operator--(int) const noexcept {
    offsetPointer t(*this);
    t -= 1;
    return *this;
  }

  friend offsetPointer operator+(difference_type diff,
                                 offsetPointer rhs) noexcept {
    rhs += diff;
    return rhs;
  }
  offsetPointer operator+(difference_type diff) const noexcept {
    offsetPointer lhs(*this);
    lhs += diff;
    return lhs;
  }

  friend offsetPointer operator-(difference_type diff,
                                 offsetPointer rhs) noexcept {
    rhs -= diff;
    return rhs;
  }
  offsetPointer operator-(difference_type diff) const noexcept {
    offsetPointer lhs(*this);
    lhs -= diff;
    return lhs;
  }
  friend difference_type operator-(const offsetPointer &lhs,
                                   const offsetPointer &rhs) {
    return lhs.rawPtr() - rhs.rawPtr();
  }

  bool operator>(const offsetPointer &rhs) const noexcept {
    return rawPtr() > rhs.rawPtr();
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator>(const offsetPointer<T2> &rhs) const noexcept {
    return rawPtr() > rhs.rawPtr();
  }
  bool operator>(pointer rhs) const noexcept { return rawPtr() > rhs; }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator>(T2 *rhs) const noexcept {
    return rawPtr() > rhs;
  }

  bool operator>=(const offsetPointer &rhs) const noexcept {
    return rawPtr() >= rhs.rawPtr();
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator>=(const offsetPointer<T2> &rhs) const noexcept {
    return rawPtr() >= rhs.rawPtr();
  }
  bool operator>=(pointer rhs) const noexcept { return rawPtr() >= rhs; }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator>=(T2 *rhs) const noexcept {
    return rawPtr() >= rhs;
  }

  bool operator<(const offsetPointer &rhs) const noexcept {
    return rawPtr() < rhs.rawPtr();
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator<(const offsetPointer<T2> &rhs) const noexcept {
    return rawPtr() < rhs.rawPtr();
  }
  bool operator<(pointer rhs) const noexcept { return rawPtr() < rhs; }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator<(T2 *rhs) const noexcept {
    return rawPtr() < rhs;
  }

  bool operator<=(const offsetPointer &rhs) const noexcept {
    return rawPtr() <= rhs.rawPtr();
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator<=(const offsetPointer<T2> &rhs) const noexcept {
    return rawPtr() <= rhs.rawPtr();
  }
  bool operator<=(pointer rhs) const noexcept { return rawPtr() <= rhs; }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator<=(T2 *rhs) const noexcept {
    return rawPtr() <= rhs;
  }

  bool operator==(const offsetPointer &rhs) const noexcept {
    return rawPtr() == rhs.rawPtr();
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator==(const offsetPointer<T2> &rhs) const noexcept {
    return rawPtr() == rhs.rawPtr();
  }
  bool operator==(pointer rhs) const noexcept { return rawPtr() == rhs; }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator==(T2 *rhs) const noexcept {
    return rawPtr() == rhs;
  }

  bool operator!=(const offsetPointer &rhs) const noexcept {
    return !operator==(rhs);
  }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator!=(const offsetPointer<T2> &rhs) const noexcept {
    return !operator==(rhs);
  }
  bool operator!=(pointer rhs) const noexcept { return !operator==(rhs); }
  template <typename T2,
            typename = std::enable_if_t<std::is_convertible_v<T2 *, pointer>>>
  bool operator!=(T2 *rhs) const noexcept {
    return !operator==(rhs);
  }

  explicit operator bool() const noexcept { return off != 0x1; }

  pointer get() const noexcept { return rawPtr(); }

  static constexpr offsetPointer pointer_to(reference r) noexcept {
    return std::addressof(r);
  }

private:
  template <typename> friend struct offsetPointer;

  void copyAssign(const offsetPointer &rhs) noexcept {
    assignRawPtr(rhs.rawPtr());
  }

  pointer rawPtr() const noexcept {
    uintptr_t null_mask = off == 0x1;
    --null_mask;
    uintptr_t raw_ptr = self() + off & null_mask;
    return reinterpret_cast<pointer>(raw_ptr);
  }

  void assignRawPtr(const void *raw_ptr) noexcept {
    uintptr_t is_null = raw_ptr == nullptr;
    uintptr_t null_mask = is_null;
    --null_mask;
    off = (uintptr_t(raw_ptr) - self() & null_mask) + is_null;
  }

  static constexpr ptrdiff_t step(ptrdiff_t i) noexcept {
    return i * sizeof(value_type);
  }

  constexpr ptrdiff_t self() const noexcept { return uintptr_t(this); }

  ptrdiff_t off = 0x1;
};

}
} // namespace db
} // namespace ccls