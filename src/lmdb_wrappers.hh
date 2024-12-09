// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "log.hh"

#include <cassert>
#include <lmdbxx/lmdb++.h>

namespace ccls {

template <typename From, typename To, typename = void> struct IsBindable {
  static constexpr bool value = false;
};
template <typename From, typename To>
struct IsBindable<From, To, std::enable_if_t<std::is_convertible_v<From, To> || std::is_constructible_v<From, To>>> {
  static constexpr bool value = true;
};

struct SubdbOperationMap {
  static constexpr MDB_cursor_op kFirst = MDB_FIRST_DUP;
  static constexpr MDB_cursor_op kLast = MDB_LAST_DUP;
  static constexpr MDB_cursor_op kPrev = MDB_PREV_DUP;
  static constexpr MDB_cursor_op kNext = MDB_NEXT_DUP;
};

struct DbOperationMap {
  static constexpr MDB_cursor_op kFirst = MDB_FIRST;
  static constexpr MDB_cursor_op kLast = MDB_LAST;
  static constexpr MDB_cursor_op kPrev = MDB_PREV;
  static constexpr MDB_cursor_op kNext = MDB_NEXT;
};

template <typename OpMap> class DbIteratorImpl {
public:
  DbIteratorImpl() = default;
  DbIteratorImpl(const lmdb::cursor &cur) : DbIteratorImpl() {
    if (cur.handle() == nullptr)
      return;
    cur_ = cur;
  }
  DbIteratorImpl(lmdb::cursor &&cur) : DbIteratorImpl() { cur_ = std::move(cur); }

  lmdb::cursor &cursor() { return cur_; }
  const lmdb::cursor &cursor() const { return cur_; }
  bool empty() const { return cur_.handle() == nullptr; }

  bool first() { return operation(OpMap::kFirst); }
  bool last() { return operation(OpMap::kLast); }
  bool prev() { return operation(OpMap::kPrev); }
  bool next() { return operation(OpMap::kNext); }

  std::pair<std::string_view, std::string_view> current() const {
    if (cur_.handle() == nullptr)
      return {};
    std::string_view k, v;
    if (!cur_.get(k, v, MDB_GET_CURRENT))
      return {};
    return {k, v};
  }

  bool equal(const DbIteratorImpl &rhs) const {
    auto [lhs_key, lhs_data] = current();
    auto [rhs_key, rhs_data] = rhs.current();
    return lhs_key.data() == rhs_key.data() && lhs_data.data() == rhs_data.data();
  }

private:
  bool operation(MDB_cursor_op op) {
    assert(op == OpMap::kFirst || op == OpMap::kLast || op == OpMap::kPrev || op == OpMap::kNext);
    if (cur_.handle() == nullptr)
      return false;
    std::string_view k, v;
    return cur_.get(k, v, op);
  }

  mutable lmdb::cursor cur_{nullptr};
};

template <typename Key, typename Value, typename IteratorImplType> class DbIterator {
  typedef IteratorImplType iterator_impl_type;

public:
  typedef Key key_type;
  typedef Value value_type;

  DbIterator() = default;
  DbIterator(const DbIterator &v) { copyAssign(v); };
  DbIterator(DbIterator &&v) { moveAssign(std::move(v)); }
  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbIterator(DbIterator<FromKey, FromValue, IteratorImplType> &&v) {
    moveAssign(std::move(v));
  }
  ~DbIterator() { iter_impl_.~iterator_impl_type(); };

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbIterator &operator=(const DbIterator<FromKey, FromValue, IteratorImplType> &rhs) {
    copyAssign(rhs);
    return *this;
  }

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbIterator &operator=(DbIterator<FromKey, FromValue, IteratorImplType> &&rhs) {
    moveAssign(std::move(rhs));
    return *this;
  }

  const value_type &operator*() { return *current(); }
  const value_type *operator->() { return current(); }

  DbIterator &operator--() {
    if (is_end_) {
      is_end_ = false;
      iter_impl_.last();
    } else {
      iter_impl_.prev();
    }
    return *this;
  }
  DbIterator operator--(int) {
    auto iter = *this;
    --iter;
    return *this;
  }

  DbIterator &operator++() {
    if (!is_end_ && !iter_impl_.next())
      is_end_ = true;
    return *this;
  }
  DbIterator operator++(int) {
    auto iter = *this;
    ++iter;
    return *this;
  }

  bool operator==(const DbIterator &rhs) const { return equal(rhs); }
  bool operator!=(const DbIterator &rhs) const { return !equal(rhs); }

private:
  template <typename, typename, typename> friend class DbIterator;
  template <typename, typename, bool> friend class DbContainer;

  DbIterator(const iterator_impl_type &cursor, bool is_end) : iter_impl_(cursor), is_end_(is_end) {}
  DbIterator(iterator_impl_type &&cursor, bool is_end) : iter_impl_(std::move(cursor)), is_end_(is_end) {}

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  void copyAssign(const DbIterator<FromKey, FromValue, IteratorImplType> &rhs) {
    if (this == static_cast<const void *>(&rhs))
      return;
    iter_impl_ = rhs.iter_impl_;
    is_end_ = rhs.is_end_;
  }

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  void moveAssign(DbIterator<FromKey, FromValue, IteratorImplType> &&rhs) {
    if (this == static_cast<const void *>(&rhs))
      return;
    iter_impl_ = std::move(rhs.iter_impl_);
    is_end_ = rhs.is_end_;
  }

  const value_type *current() {
    auto [_, data] = iter_impl_.current();
    return reinterpret_cast<const value_type *>(data.data());
  }

  bool equal(const DbIterator &it) const {
    if (iter_impl_.empty() || it.iter_impl_.empty())
      return iter_impl_.empty() == it.iter_impl_.empty();
    if (iter_impl_.cursor().txn() != it.iter_impl_.cursor().txn() ||
        iter_impl_.cursor().dbi() != it.iter_impl_.cursor().dbi())
      return false;
    if (is_end_ || it.is_end_)
      return is_end_ == it.is_end_;
    return iter_impl_.equal(it.iter_impl_);
  }

  iterator_impl_type iter_impl_;
  bool is_end_{true};
};

template <typename Key, typename Value, bool is_subdb = false> class DbContainer {
  typedef std::conditional_t<is_subdb, DbIteratorImpl<SubdbOperationMap>, DbIteratorImpl<DbOperationMap>>
      iterator_impl_type;

public:
  typedef DbIterator<Key, Value, iterator_impl_type> iterator_type;

public:
  template <bool enabled = std::is_same_v<iterator_impl_type, DbIteratorImpl<DbOperationMap>>,
            std::enable_if_t<enabled> * = nullptr>
  DbContainer(lmdb::txn &txn, lmdb::dbi &dbi) : dbi_(dbi) {
    std::string_view k;
    auto cur = lmdb::cursor::open(txn, dbi);
    if (cur.get(k, MDB_FIRST))
      base_cursor_ = std::move(cur);
    end_iterator_ = iterator_type(base_cursor_, true);
  }
  template <bool enabled = std::is_same_v<iterator_impl_type, DbIteratorImpl<SubdbOperationMap>>,
            std::enable_if_t<enabled> * = nullptr>
  DbContainer(lmdb::txn &txn, lmdb::dbi &dbi, const Key &key) : dbi_(dbi) {
    std::string_view k = lmdb::to_sv(key);
    auto cur = lmdb::cursor::open(txn, dbi);
    if (cur.get(k, MDB_SET))
      base_cursor_ = std::move(cur);
    end_iterator_ = iterator_type(base_cursor_, true);
  }
  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbContainer(DbContainer<FromKey, FromValue, is_subdb> &&rhs) {
    moveAssign(std::move(rhs));
  }
  ~DbContainer() {
    base_cursor_.~iterator_impl_type();
    end_iterator_.~iterator_type();
  }

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbContainer &operator=(const DbContainer<FromKey, FromValue, is_subdb> &rhs) {
    if (this == &rhs)
      return;
    copyAssign(rhs);
    return *this;
  }

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  DbContainer &operator=(DbContainer<FromKey, FromValue, is_subdb> &&rhs) {
    if (this == &rhs)
      return;
    moveAssign(std::move(rhs));
    return *this;
  }

  iterator_type begin() {
    if (!base_cursor_.first())
      return end();
    return iterator_type(base_cursor_, false);
  }
  iterator_type end() { return end_iterator_; }

  size_t size() {
    if (base_cursor_.empty())
      return 0;
    if constexpr (is_subdb)
      return base_cursor_.cursor().count();
    else
      return dbi_.size(base_cursor_.cursor().txn());
  }

private:
  template <typename, typename, bool> friend class DbContainer;

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  void copyAssign(const DbContainer<FromKey, FromValue, is_subdb> &rhs) {
    if (this == static_cast<const void *>(&rhs))
      return;
    dbi_ = rhs.dbi_;
    base_cursor_ = rhs.begin_iterator_;
    end_iterator_ = rhs.end_iterator_;
  }

  template <typename FromKey, typename FromValue,
            std::enable_if_t<IsBindable<FromKey, Key>::value && IsBindable<FromValue, Value>::value, bool> = true>
  void moveAssign(DbContainer<FromKey, FromValue, is_subdb> &&rhs) {
    if (this == static_cast<const void *>(&rhs))
      return;
    dbi_ = std::move(rhs.dbi_);
    base_cursor_ = std::move(rhs.base_cursor_);
    end_iterator_ = std::move(rhs.end_iterator_);
  }

  lmdb::dbi dbi_;
  iterator_impl_type base_cursor_;
  iterator_type end_iterator_;
};

template <typename Key, typename Value> using SubdbContainer = DbContainer<Key, Value, true>;
template <typename Key, typename Value> using SubdbIterator = typename SubdbContainer<Key, Value>::iterator_type;

} // namespace ccls