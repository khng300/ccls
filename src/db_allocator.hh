#pragma once

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/offset_ptr.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/sync/spin/mutex.hpp>
#include <boost/interprocess/sync/spin/recursive_mutex.hpp>

#include <cstddef>
#include <memory>
#include <pthread.h>
#include <type_traits>

namespace ccls {
namespace db {
namespace impl {

#if 0
template <typename T> class offsetPointer {
  ptrdiff_t off;

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

  offsetPointer &operator+=(difference_type diff) {
    off += step(diff);
    return *this;
  }
  offsetPointer &operator-=(difference_type diff) {
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

  explicit operator bool() const noexcept { return off != 0; }

  pointer rawPtr() const noexcept {
    uintptr_t o = off;
    uintptr_t is_null = o == 0, is_self = o == 1;
    --is_null;
    --is_self;
    o &= is_self;
    uintptr_t raw = uintptr_t(this) + o;
    raw &= is_null;
    return reinterpret_cast<pointer>(raw);
  }

  pointer get() const noexcept { return rawPtr(); }

  static constexpr offsetPointer pointer_to(reference r) noexcept {
    return std::addressof(r);
  }

private:
  void copyAssign(const offsetPointer &rhs) noexcept {
    assignRawPtr(rhs.rawPtr());
  }

  void assignRawPtr(const void *raw) noexcept {
    uintptr_t diff = uintptr_t(raw) - uintptr_t(this);
    uintptr_t self_off = diff == 0, is_null = raw == nullptr;
    --is_null;
    off = (diff + self_off) & is_null;
  }

  static constexpr ptrdiff_t step(ptrdiff_t i) {
    return i * sizeof(value_type);
  }
};
template <typename T> using offset_ptr = offsetPointer<T>;
#else
template <typename T> using offset_ptr = boost::interprocess::offset_ptr<T>;
#endif

struct MutexFamily {
  typedef boost::interprocess::ipcdetail::spin_mutex mutex_type;
  typedef boost::interprocess::ipcdetail::spin_recursive_mutex
      recursive_mutex_type;
};

using managed_mapped_file = boost::interprocess::basic_managed_mapped_file<
    char, boost::interprocess::rbtree_best_fit<MutexFamily>,
    boost::interprocess::iset_index>;
using segment_manager = boost::interprocess::segment_manager<
    char, boost::interprocess::rbtree_best_fit<MutexFamily>,
    boost::interprocess::iset_index>;
using Handle = offset_ptr<segment_manager>;

managed_mapped_file createManagedMappedFile(const std::string &db_dir);

template <class T> struct allocator {
  typedef T value_type;
  typedef offset_ptr<value_type> pointer;
  typedef offset_ptr<const value_type> const_pointer;
  typedef offset_ptr<void> void_pointer;
  typedef offset_ptr<const void> const_void_pointer;
  std::size_t size_type;
  std::ptrdiff_t difference_type;
  template <class U> struct rebind {
    typedef allocator<U> other;
  };

  allocator(Handle mgr) noexcept : mgr(mgr) {}
  allocator(const allocator &o) noexcept = default;
  template <typename U>
  allocator(const allocator<U> &o) noexcept : mgr(o.mgr) {}
  ~allocator() noexcept = default;

  allocator &operator=(const allocator &o) noexcept = default;

  pointer allocate(size_t n) {
    if (!mgr)
      return static_cast<T *>(::operator new(
          n * sizeof(T),
          static_cast<std::align_val_t>(std::alignment_of_v<T>)));
    return static_cast<T *>(
        mgr->allocate_aligned(n * sizeof(T), std::alignment_of_v<T>));
  }

  void deallocate(pointer p, size_t n) {
    if (!mgr)
      ::operator delete(p.get());
    else
      mgr->deallocate(p.get());
  }

  allocator select_on_container_copy_construction() const {
    return allocator(*this);
  }

private:
  template <typename U> friend struct allocator;
  template <typename T1, typename T2>
  friend bool operator==(const allocator<T1> &a,
                         const allocator<T2> &b) noexcept;

  allocator() noexcept = default;

  Handle mgr;
};

template <typename T1, typename T2>
bool operator==(const allocator<T1> &a, const allocator<T2> &b) noexcept {
  return a.mgr == b.mgr;
}

template <typename T1, typename T2>
bool operator!=(const allocator<T1> &a, const allocator<T2> &b) noexcept {
  return !operator==(a, b);
}

} // namespace impl
} // namespace db
} // namespace ccls