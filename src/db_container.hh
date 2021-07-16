#pragma once

#include "db_allocator.hh"

#include <boost/interprocess/containers/string.hpp>

#include <map>
#include <scoped_allocator>
#include <signal.h>
#include <unordered_map>

namespace ccls {
namespace db {

template <typename T>
using scoped_allocator = std::scoped_allocator_adaptor<allocator<T>>;

template <typename K, typename V>
using scoped_map =
    std::map<K, V, std::less<void>,
             scoped_allocator<typename std::map<K, V>::value_type>>;
template <typename K, typename V>
using scoped_unordered_map =
    std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                       scoped_allocator<typename std::map<K, V>::value_type>>;
template <typename T> using scoped_vector = std::vector<T, scoped_allocator<T>>;
using scoped_string =
    boost::interprocess::basic_string<char, std::char_traits<char>,
                                      allocator<char>>;

inline std::string toStdString(const scoped_string &s) {
  return std::string(s.begin(), s.end());
}

template <typename SV> inline scoped_string toInMemScopedString(SV &&s) {
  return scoped_string(s.begin(), s.end(), db::getAlloc());
}

template <typename Vec> class QueryVecIterator : Vec::Base::iterator {
  typedef typename Vec::Base::iterator Base;

public:
  typedef std::bidirectional_iterator_tag iterator_category;

  using value_type = typename Vec::value_type;
  using pointer_type = value_type *;
  using reference_type = value_type &;

  QueryVecIterator() : Base() {}
  QueryVecIterator(const Base &base_it) : Base(base_it) {}

  QueryVecIterator &operator--() {
    Base::operator--();
    return *this;
  }

  QueryVecIterator &operator++() {
    Base::operator++();
    return *this;
  }

  QueryVecIterator operator--(int) {
    QueryVecIterator it = *this;
    it--;
    return it;
  }

  QueryVecIterator operator++(int) {
    QueryVecIterator it = *this;
    it++;
    return it;
  }

  bool operator==(const QueryVecIterator &it) const noexcept {
    return (Base)(*this) == (Base)it;
  }
  bool operator!=(const QueryVecIterator &it) const noexcept {
    return (Base)(*this) != (Base)it;
  }

  reference_type operator*() const noexcept { return Base::operator*().second; }

  pointer_type operator->() const noexcept { return &Base::operator*().second; }
};

template <typename Vec>
class QueryVecConstIterator : Vec::Base::const_iterator {
  typedef typename Vec::Base::const_iterator Base;

public:
  typedef std::bidirectional_iterator_tag iterator_category;

  using value_type = const typename Vec::value_type;
  using pointer_type = value_type *;
  using reference_type = value_type &;

  QueryVecConstIterator() : Base() {}
  QueryVecConstIterator(const Base &base_it) : Base(base_it) {}

  QueryVecConstIterator &operator--() {
    Base::operator--();
    return *this;
  }

  QueryVecConstIterator &operator++() {
    Base::operator++();
    return *this;
  }

  QueryVecConstIterator operator--(int) {
    QueryVecIterator it = *this;
    it--;
    return it;
  }

  QueryVecConstIterator operator++(int) {
    QueryVecIterator it = *this;
    it++;
    return it;
  }

  bool operator==(const QueryVecConstIterator &it) const noexcept {
    return (Base)(*this) == (Base)it;
  }
  bool operator!=(const QueryVecConstIterator &it) const noexcept {
    return (Base)(*this) != (Base)it;
  }

  reference_type operator*() const noexcept { return Base::operator*().second; }

  pointer_type operator->() const noexcept { return &Base::operator*().second; }
};

template <typename V> struct QueryVec : private scoped_map<size_t, V> {
  typedef V value_type;
  typedef V &reference_type;
  typedef QueryVecIterator<QueryVec> iterator;
  typedef QueryVecConstIterator<QueryVec> const_iterator;

  QueryVec(const allocator<QueryVec> &alloc) : scoped_map<size_t, V>(alloc) {}

  void push_back(const value_type &v) {
    Base::insert_or_assign(std::make_pair(Base::size(), v));
  }

  template <class... Args> reference_type emplace_back(Args &&... args) {
    return Base::try_emplace(Base::size(), args...).first->second;
  }

  size_t size() const noexcept { return Base::size(); }

  void clear() { Base::clear(); }

  value_type &operator[](size_t i) {
    auto it = Base::find(i);
    if (it == Base::end()) {
      raise(SIGSTOP);
      throw std::out_of_range("QueryVec: out of range");
    }
    return it->second;
  }

  const value_type &operator[](size_t i) const {
    auto it = Base::find(i);
    if (it == Base::end()) {
      raise(SIGSTOP);
      throw std::out_of_range("QueryVec: out of range");
    }
    return it->second;
  }

  iterator begin() noexcept { return Base::begin(); }
  iterator end() noexcept { return Base::end(); }
  const_iterator begin() const noexcept { return Base::begin(); }
  const_iterator end() const noexcept { return Base::end(); }
  const_iterator cbegin() const noexcept { return Base::cbegin(); }
  const_iterator cend() const noexcept { return Base::cend(); }

private:
  friend QueryVecIterator<QueryVec>;
  friend QueryVecConstIterator<QueryVec>;

  typedef scoped_map<size_t, V> Base;
};
} // namespace db
} // namespace ccls

namespace std {
template <> struct hash<ccls::db::scoped_string> {
  size_t operator()(const ccls::db::scoped_string &t) const _NOEXCEPT {
    return boost::container::hash_value(t);
  }
};
} // namespace std