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

template <typename T> using offset_ptr = boost::interprocess::offset_ptr<T>;

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
  template <class U> struct rebind { typedef allocator<U> other; };

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
  template <typename T1> friend allocator<T1> getAlloc(Handle mgr);

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

template <typename T1> allocator<T1> getAlloc(Handle mgr) {
  allocator<T1> res;
  res.mgr = mgr;
  return res;
}

} // namespace impl
} // namespace db
} // namespace ccls