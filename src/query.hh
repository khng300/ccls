// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "indexer.hh"
#include "log.hh"
#include "working_files.hh"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <lmdbxx/lmdb++.h>

#include <array>
#include <iterator>

namespace llvm {
template <> struct DenseMapInfo<ccls::ExtentRef> {
  static inline ccls::ExtentRef getEmptyKey() { return {}; }
  static inline ccls::ExtentRef getTombstoneKey() {
    return {{ccls::Range(), ccls::Usr(-1)}};
  }
  static unsigned getHashValue(ccls::ExtentRef sym) {
    return std::hash<ccls::ExtentRef>()(sym);
  }
  static bool isEqual(ccls::ExtentRef l, ccls::ExtentRef r) { return l == r; }
};
} // namespace llvm

namespace ccls {
template <typename Key, typename Value> struct SubdbCursor {
  typedef Key key_type;
  typedef Value value_type;

  template <typename TypeA, typename TypeB, typename = void> struct Bindable {
    static constexpr bool value = false;
  };
  template <typename TypeA, typename TypeB>
  struct Bindable<TypeA, TypeB,
                  std::enable_if_t<std::is_base_of_v<TypeA, TypeB> ||
                                   std::is_base_of_v<TypeB, TypeA>>> {
    static constexpr bool value = true;
  };
  template <typename TypeA, typename TypeB>
  static constexpr bool IsBindable = Bindable<TypeA, TypeB>::value;

  // Default constructor is a special end marker
  SubdbCursor() = default;
  SubdbCursor(const SubdbCursor &it) = delete;
  SubdbCursor(SubdbCursor &&it) : SubdbCursor() { moveAssign(std::move(it)); }
  template <
      typename FromKey, typename FromValue,
      std::enable_if_t<IsBindable<FromKey, Key> && IsBindable<FromValue, Value>,
                       bool> = true>
  SubdbCursor(SubdbCursor<FromKey, FromValue> &&it) : SubdbCursor() {
    moveAssign(std::move(it));
  }
  SubdbCursor(lmdb::cursor &&cur) : SubdbCursor() { cur_ = std::move(cur); }

  static auto makeCursor(lmdb::txn &txn, lmdb::dbi &dbi, const Key &key) {
    return makeCursor(txn, dbi, &key);
  }

  SubdbCursor &operator=(const SubdbCursor &it) = delete;
  SubdbCursor &operator=(SubdbCursor &&it) {
    moveAssign(std::move(it));
    return *this;
  }
  template <
      typename FromKey, typename FromValue,
      std::enable_if_t<IsBindable<FromKey, Key> && IsBindable<FromValue, Value>,
                       bool> = true>
  SubdbCursor &operator=(SubdbCursor<FromKey, FromValue> &&it) {
    moveAssign(std::move(it));
    return *this;
  }

  operator bool() { return cur_.handle() != nullptr; }

  const key_type *key() {
    std::string_view key;
    if (!cur_.get(key, MDB_GET_CURRENT))
      return nullptr;
    return lmdb::ptr_from_sv<value_type>(key);
  }

  const value_type &operator*() { return *current(); }
  const value_type *operator->() {
    auto [key, val] = current();
    return val;
  }

  const value_type *first() { return cursorOperation(MDB_FIRST_DUP); }
  const value_type *prev() { return cursorOperation(MDB_PREV_DUP); }
  const value_type *next() { return cursorOperation(MDB_NEXT_DUP); }
  const value_type *current() { return cursorOperation(MDB_GET_CURRENT); }

  void del() { cur_.del(); }

private:
  template <typename, typename> friend struct SubdbCursor;

  static auto makeCursor(lmdb::txn &txn, lmdb::dbi &dbi, const Key *key) {
    auto cur = lmdb::cursor::open(txn, dbi);
    std::string_view k = lmdb::to_sv(*key);
    if (!cur.get(k, MDB_SET))
      return SubdbCursor();
    return SubdbCursor(std::move(cur));
  }

  void moveAssign(SubdbCursor &&rhs) {
    if (this == &rhs)
      return;
    std::swap(cur_, rhs.cur_);
  }
  template <
      typename FromKey, typename FromValue,
      std::enable_if_t<IsBindable<FromKey, Key> && IsBindable<FromValue, Value>,
                       bool> = true>
  void moveAssign(SubdbCursor<FromKey, FromValue> &&rhs) {
    if (this == static_cast<void *>(&rhs))
      return;
    std::swap(cur_, rhs.cur_);
  }

  const value_type *cursorOperation(MDB_cursor_op op) {
    std::string_view key, val;
    if (!cur_.get(key, val, op))
      return nullptr;
    return lmdb::ptr_from_sv<value_type>(val);
  }

  mutable lmdb::cursor cur_{nullptr};
};

#define CCLS_CURSOR_FOREACH(_cur, val_name)                                    \
  if (auto &&__cur = (_cur); true)                                             \
    for (const auto *val_name##_p = !!(__cur) ? (__cur).first() : nullptr;     \
         (val_name##_p) != nullptr; (val_name##_p) = (__cur).next())           \
      if (const auto &val_name = *(val_name##_p); true)

enum class ObjId : uint64_t;

template <typename T> using IUVector = llvm::SmallVector<T, 0>;
using IUFuncDef = FuncDef<IUVector>;
using IUTypeDef = TypeDef<IUVector>;
using IUVarDef = VarDef;

struct FileDef {
  struct IndexInclude {
    int line = 0;
    std::string resolved_path;
  };
  std::string path;
  std::vector<std::string> args;
  LanguageId language;
  // Includes in the file.
  std::vector<IndexInclude> includes;
  // Parts of the file which are disabled.
  std::vector<Range> skipped_ranges;
  // Used by |$ccls/reload|.
  std::vector<std::string> dependencies;
  // modification time of the file recorded
  int64_t mtime = 0;
};

struct QueryFile {
  using Def = FileDef;
  using DefUpdate = std::pair<FileDef, std::string>;

  int id = -1;
  std::optional<Def> def;
  // `extent` is valid => declaration; invalid => regular reference
  std::unordered_map<ExtentRef, int> symbol2refcnt;
};
REFLECT_UNDERLYING_B(SymbolKind);

constexpr ObjId InvalidObjId{-1ull};

struct DB;
struct QueryContainer {
  ObjId obj_id = InvalidObjId;

  std::string_view get(const DB *db) const;
  void put(DB *db, std::string_view val);
};

struct QueryString : QueryContainer {
  std::string_view get(const DB *db) const {
    std::string_view r;
    r = QueryContainer::get(db);
    if (r.empty())
      throw std::runtime_error("Malformed QueryString");
    r.remove_suffix(1);
    return r;
  }

  void put(DB *db, std::string_view val) {
    std::string v(val);
    v.append(1, '\0');
    QueryContainer::put(db, v);
  }
};

template <typename T> struct QueryVector : QueryContainer {
  llvm::ArrayRef<T> get(const DB *db) const {
    llvm::ArrayRef<T> r;
    std::string_view val = QueryContainer::get(db);
    r = llvm::ArrayRef<T>(reinterpret_cast<const T *>(val.data()),
                          val.size() / sizeof(T));
    return r;
  }

  void put(DB *db, llvm::ArrayRef<T> v) {
    QueryContainer::put(
        db, std::string_view(reinterpret_cast<const char *>(v.data()),
                             v.size() * sizeof(T)));
  }
};

struct alignas(16) EntityID {
  Kind kind;
  Usr usr;
};
static EntityID makeEntityID(Kind kind, Usr usr) { return EntityID{kind, usr}; }

struct alignas(32) QueryEntity {
  EntityID id;
  Usr usr;
  Kind kind;
};

struct alignas(16) QueryEntityDef {
  // General metadata.
  QueryString detailed_name;
  QueryString hover;
  QueryString comments;
  Maybe<DeclRef> spell;

  int file_id = -1;
  int16_t qual_name_offset = 0;
  int16_t short_name_offset = 0;
  int16_t short_name_size = 0;

  SymbolKind kind = SymbolKind::Unknown;
  SymbolKind parent_kind = SymbolKind::Unknown;
};

struct alignas(16) QueryFuncDef : QueryEntityDef, NameMixin<QueryFuncDef> {
  // Method this method overrides.
  QueryVector<Usr> bases;
  // Local variables or parameters.
  QueryVector<Usr> vars;
  // Functions that this function calls.
  QueryVector<SymbolRef> callees;

  uint8_t storage = clang::SC_None;
};

struct alignas(16) QueryTypeDef : QueryEntityDef, NameMixin<QueryTypeDef> {
  QueryVector<Usr> bases;
  // Types, functions, and variables defined in this type.
  QueryVector<Usr> funcs;
  QueryVector<Usr> types;
  QueryVector<std::pair<Usr, int64_t>> vars;

  // If set, then this is the same underlying type as the given value (ie, this
  // type comes from a using or typedef statement).
  Usr alias_of = 0;
};

struct alignas(16) QueryVarDef : QueryEntityDef, NameMixin<QueryVarDef> {
  QueryVector<Usr> bases;
  // Type of the variable.
  Usr type = 0;
  // Note a variable may have instances of both |None| and |Extern|
  // (declaration).
  uint8_t storage = clang::SC_None;

  bool is_local() const {
    return spell &&
           (parent_kind == SymbolKind::Function ||
            parent_kind == SymbolKind::Method ||
            parent_kind == SymbolKind::StaticMethod ||
            parent_kind == SymbolKind::Constructor) &&
           (storage == clang::SC_None || storage == clang::SC_Auto ||
            storage == clang::SC_Register);
  }
};

template <typename _EntityType, typename QDef>
struct QueryObject : QueryEntity {
  using EntityType = _EntityType;
  using Def = QDef;

  SubdbCursor<EntityID, Def> defCursor(const DB &db) const;
  SubdbCursor<EntityID, DeclRef> declCursor(const DB &db) const;
  SubdbCursor<EntityID, Use> useCursor(const DB &db) const;

  std::optional<Def> getFirstDef(const DB &db) const;
  std::optional<Def> getAnyDef(const DB &db) const;
};

struct QueryFunc : QueryObject<QueryFunc, QueryFuncDef> {
  SubdbCursor<EntityID, Usr> derivedCursor(const DB &db) const;
};
struct QueryType : QueryObject<QueryType, QueryTypeDef> {
  SubdbCursor<EntityID, Usr> derivedCursor(const DB &db) const;
  SubdbCursor<EntityID, Usr> instanceCursor(const DB &db) const;
};
struct QueryVar : QueryObject<QueryVar, QueryVarDef> {};

template <typename _T> static constexpr Kind DefToKind() {
  using T = typename std::remove_cv_t<std::remove_reference_t<_T>>;
  if constexpr (std::is_same_v<T, QueryFunc::Def> ||
                std::is_same_v<T, IUFuncDef>)
    return Kind::Func;
  else if constexpr (std::is_same_v<T, QueryType::Def> ||
                     std::is_same_v<T, IUTypeDef>)
    return Kind::Type;
  else if constexpr (std::is_same_v<T, QueryVar::Def> ||
                     std::is_same_v<T, IUVarDef>)
    return Kind::Var;
}

template <typename _T> static constexpr Kind EntityToKind() {
  using T = typename std::remove_cv_t<std::remove_reference_t<_T>>;
  if constexpr (std::is_same_v<T, QueryFunc>)
    return Kind::Func;
  else if constexpr (std::is_same_v<T, QueryType>)
    return Kind::Type;
  else if constexpr (std::is_same_v<T, QueryVar>)
    return Kind::Var;
}

template <Kind kind> struct KindToEntity {};
template <> struct KindToEntity<Kind::Func> {
  using type = QueryFunc;
};
template <> struct KindToEntity<Kind::Type> {
  using type = QueryType;
};
template <> struct KindToEntity<Kind::Var> {
  using type = QueryVar;
};
template <Kind kind> using QueryEntityType = typename KindToEntity<kind>::type;

template <typename T>
using Update =
    std::unordered_map<Usr, std::pair<std::vector<T>, std::vector<T>>>;

struct IndexUpdate {
  // Creates a new IndexUpdate based on the delta from previous to current. If
  // no delta computation should be done just pass null for previous.
  static IndexUpdate createDelta(IndexFile *previous, IndexFile *current);

  int file_id;

  // Dummy one to refresh all semantic highlight.
  bool refresh = false;

  decltype(IndexFile::lid2path) prev_lid2path;
  decltype(IndexFile::lid2path) lid2path;

  // File updates.
  std::optional<std::string> files_removed;
  std::optional<QueryFile::DefUpdate> files_def_update;

  // Function updates.
  int funcs_hint;
  std::vector<std::pair<Usr, IUFuncDef>> funcs_removed;
  std::vector<std::pair<Usr, IUFuncDef>> funcs_def_update;
  Update<DeclRef> funcs_declarations;
  Update<Use> funcs_uses;
  Update<Usr> funcs_derived;

  // Type updates.
  int types_hint;
  std::vector<std::pair<Usr, IUTypeDef>> types_removed;
  std::vector<std::pair<Usr, IUTypeDef>> types_def_update;
  Update<DeclRef> types_declarations;
  Update<Use> types_uses;
  Update<Usr> types_derived;
  Update<Usr> types_instances;

  // Variable updates.
  int vars_hint;
  std::vector<std::pair<Usr, VarDef>> vars_removed;
  std::vector<std::pair<Usr, VarDef>> vars_def_update;
  Update<DeclRef> vars_declarations;
  Update<Use> vars_uses;
};

struct DenseMapInfoForUsr {
  static inline Usr getEmptyKey() { return 0; }
  static inline Usr getTombstoneKey() { return ~0ULL; }
  static unsigned getHashValue(Usr w) { return w; }
  static bool isEqual(Usr l, Usr r) { return l == r; }
};

using Lid2file_id = std::unordered_map<int, int>;

struct QueryStore {
  QueryStore() : env(nullptr) {}
  QueryStore(std::string_view store_path);

  void increaseMapSize();

private:
  friend struct DB;
  friend struct TxnDB;
  friend struct QueryContainer;
  friend struct QueryFunc;
  friend struct QueryType;
  friend struct QueryVar;

  lmdb::env env;

  lmdb::dbi dbi_entities_objects; // Files/Entities-related object store
  lmdb::dbi dbi_entities_objects_freelist;
  lmdb::dbi dbi_files;
  lmdb::dbi dbi_hashed_name2file_id;

  //
  // Entities database:
  //
  // There are one main database plus 5 properties databases:
  // - entities: QueryEntity storage
  // - entities_def: QueryFunc/QueryType/QueryVar definition storage. Value is
  //                 Query*Def
  // - entities_declarations: QueryFunc/QueryType/QueryVar declaration storage.
  //                          Value is DeclRef
  // - entities_derived: QueryFunc/QueryType derived storage. Value is Usr.
  // - entities_instances: QueryType derived storage. Value is Usr.
  // - entities_uses: QueryFunc/QueryType/QueryVar derived storage. Value is
  //                  Use.
  //
  lmdb::dbi entities;
  lmdb::dbi entities_defs;
  lmdb::dbi entities_declarations;
  lmdb::dbi entities_derived;
  lmdb::dbi entities_instances;
  lmdb::dbi entities_uses;
  lmdb::dbi kind_to_entity_id;
};

struct DB {
  DB(QSConnection qs, lmdb::txn &txn) : qs(qs), txn(txn) {}

  void clear();
  // Insert the contents of |update| into |db|.
  void applyIndexUpdate(IndexUpdate *update);

  void populateVFS(VFS *vfs) const;

  std::optional<int> findFileId(const std::string &path) const;
  std::string getSymbolName(SymbolIdx sym, bool qualified) const;
  std::vector<uint8_t>
  getFileSet(const std::vector<std::string> &folders) const;

  bool hasEntityId(Kind kind, Usr usr) const;
  bool hasFunc(Usr usr) const { return hasEntityId(Kind::Func, usr); }
  bool hasType(Usr usr) const { return hasEntityId(Kind::Type, usr); }
  bool hasVar(Usr usr) const { return hasEntityId(Kind::Var, usr); }

  const QueryEntity &id2Entity(EntityID id) const;
  template <typename EntityType> const auto &id2Entity(EntityID id) const {
    return static_cast<const EntityType &>(id2Entity(id));
  }
  const QueryFunc &id2Func(EntityID id) const {
    return id2Entity<QueryFunc>(id);
  }
  const QueryType &id2Type(EntityID id) const {
    return id2Entity<QueryType>(id);
  }
  const QueryVar &id2Var(EntityID id) const { return id2Entity<QueryVar>(id); }

  const QueryFunc &getFunc(Usr usr) const {
    return id2Func(makeEntityID(Kind::Func, usr));
  }
  const QueryType &getType(Usr usr) const {
    return id2Type(makeEntityID(Kind::Type, usr));
  }
  const QueryVar &getVar(Usr usr) const {
    return id2Var(makeEntityID(Kind::Var, usr));
  }

  QueryFile getFile(int file_id) const;
  const QueryFunc &getFunc(SymbolIdx ref) const { return getFunc(ref.usr); }
  const QueryType &getType(SymbolIdx ref) const { return getType(ref.usr); }
  const QueryVar &getVar(SymbolIdx ref) const { return getVar(ref.usr); }

  int files_count() const { return qs->dbi_files.size(txn); }
  template <Kind kind> uint64_t entities_count() const {
    lmdb::cursor cur = lmdb::cursor::open(txn, qs->kind_to_entity_id);
    std::string_view k = lmdb::to_sv(kind);
    if (!cur.get(k, MDB_SET))
      return 0;
    return cur.count();
  }
  uint64_t funcs_count() const { return entities_count<Kind::Func>(); }
  uint64_t types_count() const { return entities_count<Kind::Type>(); }
  uint64_t vars_count() const { return entities_count<Kind::Var>(); }

  void foreachFile(std::function<bool(const QueryFile &)> &) const;
  template <typename Fn> void foreachFile(Fn &&fn) const {
    std::function<bool(const QueryFile &)> erasure =
        [&](const QueryFile &f) -> bool { return fn(f); };
    foreachFile(erasure);
  }

  SubdbCursor<EntityID, QueryEntityDef>
  entityDefCursor(const QueryEntity &entity) const;
  SubdbCursor<EntityID, DeclRef>
  entityDeclCursor(const QueryEntity &entity) const;
  SubdbCursor<EntityID, Usr>
  entityDerivedCursor(const QueryEntity &entity) const;
  SubdbCursor<EntityID, Usr>
  entityInstanceCursor(const QueryEntity &entity) const;
  SubdbCursor<EntityID, Use> entityUseCursor(const QueryEntity &entity) const;

  template <typename Fn> bool allOfUsr(Kind kind, Fn &&fn) const {
    auto cursor = SubdbCursor<Kind, EntityID>::makeCursor(
        txn, qs->kind_to_entity_id, kind);
    CCLS_CURSOR_FOREACH(cursor, id) {
      if (!fn(id))
        return false;
    }
    return true;
  }

  template <typename ET> auto entityGetFirstDef(ET &&entity) const {
    using _ET = typename std::remove_cv_t<std::remove_reference_t<ET>>;
    auto cursor = entity.defCursor(*this);
    const typename _ET::Def *def = cursor.first();
    if (def != nullptr)
      return std::optional<typename _ET::Def>(*def);
    return std::optional<typename _ET::Def>();
  }

  template <typename ET> auto entityGetAnyDef(ET &&entity) const {
    using _ET = typename std::remove_cv_t<std::remove_reference_t<ET>>;
    std::optional<typename _ET::Def> ret;
    auto cursor = entity.defCursor(*this);
    CCLS_CURSOR_FOREACH(cursor, def) {
      ret = def;
      if (ret->spell)
        break;
    }
    return ret;
  }

  size_t dbiSubCount(lmdb::txn &txn, lmdb::dbi &dbi,
                     std::string_view key) const;

  template <typename ET> size_t defCount(ET &&e) const {
    return dbiSubCount(txn, qs->entities_defs, lmdb::to_sv(e.id));
  }
  template <typename ET> size_t declCount(ET &&e) const {
    return dbiSubCount(txn, qs->entities_declarations, lmdb::to_sv(e.id));
  }
  template <typename ET> size_t derivedCount(ET &&e) const {
    return dbiSubCount(txn, qs->entities_derived, lmdb::to_sv(e.id));
  }
  template <typename ET> size_t instancesCount(ET &&e) const {
    return dbiSubCount(txn, qs->entities_instances, lmdb::to_sv(e.id));
  }
  template <typename ET> size_t usesCount(ET &&e) const {
    return dbiSubCount(txn, qs->entities_uses, lmdb::to_sv(e.id));
  }

private:
  int getFileId(const std::string &path);
  void putFile(int file_id, QueryFile &file);

  ObjId allocObjId();
  void removeObj(ObjId obj_id);

  QueryFunc::Def allocate(const IUFuncDef &o, const QueryFunc::Def &existing);
  QueryType::Def allocate(const IUTypeDef &o, const QueryType::Def &existing);
  QueryVar::Def allocate(const IUVarDef &o, const QueryVar::Def &existing);

  template <typename ET, typename Def> void insertEntityDef(ET &&e, Def &&def);
  template <typename Def> void removeEntityDef(SubdbCursor<EntityID, Def> &it);

  template <Kind kind>
  std::pair<QueryEntityType<kind>, bool> idPutEntity(Usr usr);
  std::pair<QueryEntity, bool> idPutEntity(Kind kind, Usr usr);

  void idRemoveEntity(SubdbCursor<EntityID, QueryEntity> &it);

  template <typename Def>
  void removeUsrs(int file_id,
                  const std::vector<std::pair<Usr, Def>> &to_remove);
  template <typename Key, typename Value>
  std::remove_reference_t<Value> *
  insertMap(lmdb::dbi &dbi, Key &&key, Value &&val, bool no_overwrite = false);
  template <typename Key, typename Value>
  bool removeMap(lmdb::dbi &dbi, Key &&key, Value &&val);

  int update(std::unordered_map<int, QueryFile> &, QueryFile::DefUpdate &&u);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, IUTypeDef>> &&us);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, IUFuncDef>> &&us);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, IUVarDef>> &&us);

  friend struct QueryStore;
  friend struct QueryContainer;
  friend struct QueryFunc;
  friend struct QueryType;
  friend struct QueryVar;

  QSConnection qs;
  lmdb::txn &txn;
};

struct TxnDB : lmdb::txn {
  TxnDB() : lmdb::txn(nullptr) {}

  static TxnDB begin(QSConnection qs, bool read_only) {
    lmdb::txn txn(nullptr);
    do {
      try {
        txn = lmdb::txn::begin(qs->env, nullptr, read_only ? MDB_RDONLY : 0);
      } catch (const lmdb::runtime_error &e) {
        if (e.code() != MDB_MAP_RESIZED)
          throw;
        qs->env.set_mapsize(0);
      }
    } while (txn.handle() == nullptr);
    return TxnDB(qs, std::move(txn));
  }

  void commit() {
    lmdb::txn::commit();
    _db.reset();
  }

  void abort() noexcept {
    lmdb::txn::abort();
    _db.reset();
  }

  DB *db() { return _db.has_value() ? &_db.value() : nullptr; }

private:
  TxnDB(QSConnection qs, lmdb::txn &&txn)
      : lmdb::txn(std::move(txn)), qs(qs), _db(DB(qs, *this)) {}

  QSConnection qs;
  std::optional<DB> _db;
};

template <typename _EntityType, typename QDef>
SubdbCursor<EntityID, QDef>
QueryObject<_EntityType, QDef>::defCursor(const DB &db) const {
  return db.entityDefCursor(static_cast<const EntityType &>(*this));
}

template <typename _EntityType, typename QDef>
SubdbCursor<EntityID, DeclRef>
QueryObject<_EntityType, QDef>::declCursor(const DB &db) const {
  return db.entityDeclCursor(static_cast<const EntityType &>(*this));
}

template <typename _EntityType, typename QDef>
SubdbCursor<EntityID, Use>
QueryObject<_EntityType, QDef>::useCursor(const DB &db) const {
  return db.entityUseCursor(static_cast<const EntityType &>(*this));
}

template <typename _EntityType, typename QDef>
std::optional<QDef>
QueryObject<_EntityType, QDef>::getFirstDef(const DB &db) const {
  return db.entityGetFirstDef(static_cast<const EntityType &>(*this));
}

template <typename _EntityType, typename QDef>
std::optional<QDef>
QueryObject<_EntityType, QDef>::getAnyDef(const DB &db) const {
  return db.entityGetAnyDef(static_cast<const EntityType &>(*this));
}

Maybe<DeclRef> getDefinitionSpell(DB *db, SymbolIdx sym);

// Get defining declaration (if exists) or an arbitrary declaration (otherwise)
// for each id.
std::vector<Use> getFuncDeclarations(DB *, llvm::ArrayRef<Usr>);
std::vector<Use> getTypeDeclarations(DB *, llvm::ArrayRef<Usr>);
std::vector<DeclRef> getVarDeclarations(DB *, llvm::ArrayRef<Usr>, unsigned);

std::vector<Use> getFuncDeclarations(DB *, SubdbCursor<EntityID, Usr>);
std::vector<Use> getTypeDeclarations(DB *, SubdbCursor<EntityID, Usr>);
std::vector<DeclRef> getVarDeclarations(DB *, SubdbCursor<EntityID, Usr>,
                                        unsigned);

// Get non-defining declarations.
std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym);

std::vector<Use> getUsesForAllBases(DB *db, const QueryFunc &root);
std::vector<Use> getUsesForAllDerived(DB *db, const QueryFunc &root);
std::optional<lsRange> getLsRange(WorkingFile *working_file,
                                  const Range &location);
DocumentUri getLsDocumentUri(DB *db, int file_id, std::string *path);
DocumentUri getLsDocumentUri(DB *db, int file_id);

std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles, Use use);
std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles,
                                      SymbolRef sym, int file_id);
LocationLink getLocationLink(DB *db, WorkingFiles *wfiles, DeclRef dr);

// Returns a symbol. The symbol will *NOT* have a location assigned.
std::optional<SymbolInformation> getSymbolInfo(DB *db, SymbolIdx sym,
                                               bool detailed);

std::vector<SymbolRef> findSymbolsAtLocation(WorkingFile *working_file,
                                             const QueryFile *file,
                                             Position &ls_pos,
                                             bool smallest = false);

template <typename Fn> void withEntity(DB *db, SymbolIdx sym, Fn &&fn) {
  switch (sym.kind) {
  case Kind::Invalid:
  case Kind::File:
    break;
  case Kind::Func:
    fn(db->getFunc(sym));
    break;
  case Kind::Type:
    fn(db->getType(sym));
    break;
  case Kind::Var:
    fn(db->getVar(sym));
    break;
  }
}

template <typename Fn> void eachEntityDef(DB *db, SymbolIdx sym, Fn &&fn) {
  withEntity(db, sym, [&](const auto &entity) {
    auto cursor = entity.defCursor(*db);
    CCLS_CURSOR_FOREACH(cursor, def) {
      if (!fn(def))
        break;
    }
  });
}

template <typename Fn>
void eachOccurrence(DB *db, SymbolIdx sym, bool include_decl, Fn &&fn) {
  withEntity(db, sym, [&](const auto &entity) {
    auto use_cursor = entity.useCursor(*db);
    CCLS_CURSOR_FOREACH(use_cursor, use) { fn(use); }
    if (include_decl) {
      auto def_cursor = entity.defCursor(*db);
      auto decl_cursor = entity.declCursor(*db);
      CCLS_CURSOR_FOREACH(def_cursor, def) {
        if (def.spell)
          fn(*def.spell);
      }
      CCLS_CURSOR_FOREACH(decl_cursor, dr) { fn(dr); }
    }
  });
}

SymbolKind getSymbolKind(DB *db, SymbolIdx sym);

template <typename ContainerType, typename Fn>
bool allOf(ContainerType &&container, Fn &&fn) {
  if constexpr (is_iterable_v<ContainerType>) {
    for (auto it = std::begin(container), end_it = std::end(container);
         it != end_it; ++it) {
      if (!fn(*it))
        return false;
    }
  } else {
    CCLS_CURSOR_FOREACH(container, v) {
      if (!fn(v))
        return false;
    }
  }
  return true;
}

template <typename C, typename Fn>
void eachDefinedFunc(DB *db, C &&usrs, Fn &&fn) {
  allOf(usrs, [&](auto usr) {
    const auto &obj = db->getFunc(usr);
    if (db->defCount(obj) != 0)
      fn(obj);
    return true;
  });
}

} // namespace ccls