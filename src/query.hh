// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "indexer.hh"
#include "log.hh"
#include "working_files.hh"

#include <cista/serialization.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <lmdbxx/lmdb++.h>

#include <array>
#include <iterator>
#include <type_traits>

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
enum class ObjId : uint64_t;
constexpr ObjId InvalidObjId{-1ull};

struct DB;
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
  SubdbCursor(DB *db, lmdb::cursor &&cur) : SubdbCursor() {
    this->db = db;
    cur_ = std::move(cur);
  }

  static auto makeCursor(DB *db, lmdb::txn &txn, lmdb::dbi &dbi,
                         const Key &key) {
    return makeCursor(db, txn, dbi, &key);
  }
  static auto makeCursor(const DB *db, lmdb::txn &txn, lmdb::dbi &dbi,
                         const Key &key) {
    return makeCursor(const_cast<DB *>(db), txn, dbi, &key);
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
    return lmdb::ptr_from_sv<key_type>(key);
  }

  const value_type &operator*() { return *current(); }
  const value_type *operator->() { return current(); }

  const value_type *first() { return cursorOperation(MDB_FIRST_DUP); }
  const value_type *prev() { return cursorOperation(MDB_PREV_DUP); }
  const value_type *next() { return cursorOperation(MDB_NEXT_DUP); }
  const value_type *current() { return cursorOperation(MDB_GET_CURRENT); }

  size_t count() { return cur_.count(); }

  void del() { cur_.del(); }

private:
  template <typename, typename> friend struct SubdbCursor;

  static auto makeCursor(DB *db, lmdb::txn &txn, lmdb::dbi &dbi,
                         const Key *key) {
    auto cur = lmdb::cursor::open(txn, dbi);
    std::string_view k = lmdb::to_sv(*key);
    if (!cur.get(k, MDB_SET))
      return SubdbCursor();
    return SubdbCursor(db, std::move(cur));
  }

  void moveAssign(SubdbCursor &&rhs) {
    if (this == &rhs)
      return;
    std::swap(db, rhs.db);
    std::swap(cur_, rhs.cur_);
  }
  template <
      typename FromKey, typename FromValue,
      std::enable_if_t<IsBindable<FromKey, Key> && IsBindable<FromValue, Value>,
                       bool> = true>
  void moveAssign(SubdbCursor<FromKey, FromValue> &&rhs) {
    if (this == static_cast<void *>(&rhs))
      return;
    std::swap(db, rhs.db);
    std::swap(cur_, rhs.cur_);
  }

  const typename SubdbCursor<Key, Value>::value_type *
  cursorOperation(MDB_cursor_op op) {
    std::string_view key, val;
    if (!cur_.get(key, val, op))
      return nullptr;
    return lmdb::ptr_from_sv<value_type>(val);
  }

  DB *db;
  mutable lmdb::cursor cur_{nullptr};
};

#define CCLS_CURSOR_FOREACH(_cur, val_name)                                    \
  if (auto &&__cur = (_cur); true)                                             \
    for (const auto *val_name##_p = !!(__cur) ? (__cur).first() : nullptr;     \
         (val_name##_p) != nullptr; (val_name##_p) = (__cur).next())           \
      if (const auto &val_name = *(val_name##_p); true)

namespace cista_types = cista::offset;

struct FileDef {
  struct IndexInclude {
    int line = 0;
    cista_types::cstring resolved_path;
  };
  cista_types::cstring path;
  cista_types::vector<cista_types::cstring> args;
  LanguageId language;
  // Includes in the file.
  cista_types::vector<IndexInclude> includes;
  // Parts of the file which are disabled.
  cista_types::vector<Range> skipped_ranges;
  // Used by |$ccls/reload|.
  cista_types::vector<cista_types::cstring> dependencies;
  // modification time of the file recorded
  int64_t mtime = 0;
};

struct QueryFile {
  using Def = FileDef;
  using DefUpdate = std::pair<FileDef, std::string>;

  int id = -1;
  cista::optional<Def> def;
  // `extent` is valid => declaration; invalid => regular reference
  cista_types::hash_map<ExtentRef, int> symbol2refcnt;
};
REFLECT_UNDERLYING_B(SymbolKind);

struct alignas(16) EntityID {
  union {
    struct {
      Kind kind;
      Usr usr;
    };
    uint8_t blob[16]{};
  };
};
inline EntityID makeEntityID(Kind kind, Usr usr) {
  EntityID id;
  id.kind = kind;
  id.usr = usr;
  return id;
}

struct alignas(32) QueryEntity {
  EntityID id;
  Usr usr;
  Kind kind;
};

struct alignas(16) QueryEntityDef {
  // General metadata.
  cista_types::cstring detailed_name;
  cista_types::cstring hover;
  cista_types::cstring comments;
  Maybe<DeclRef> spell;

  int file_id = -1;
  int16_t qual_name_offset = 0;
  int16_t short_name_offset = 0;
  int16_t short_name_size = 0;

  SymbolKind kind = SymbolKind::Unknown;
  SymbolKind parent_kind = SymbolKind::Unknown;

  auto cista_members() {
    return std::tie(detailed_name, hover, comments,
                    static_cast<Ref &>(spell.storage), spell.storage.file_id,
                    spell.storage.extent, file_id, qual_name_offset,
                    short_name_offset, short_name_size, kind, parent_kind);
  }
};

struct alignas(16) QueryFuncDef : QueryEntityDef, NameMixin<QueryFuncDef> {
  // Method this method overrides.
  cista_types::vector<Usr> bases;
  // Local variables or parameters.
  cista_types::vector<Usr> vars;
  // Functions that this function calls.
  cista_types::vector<SymbolRef> callees;

  uint8_t storage = clang::SC_None;

  auto cista_members() {
    return std::tie(static_cast<QueryEntityDef &>(*this), bases, vars, callees,
                    storage);
  }
};

struct alignas(16) QueryTypeDef : QueryEntityDef, NameMixin<QueryTypeDef> {
  cista_types::vector<Usr> bases;
  // Types, functions, and variables defined in this type.
  cista_types::vector<Usr> funcs;
  cista_types::vector<Usr> types;
  cista_types::vector<std::pair<Usr, int64_t>> vars;

  // If set, then this is the same underlying type as the given value (ie, this
  // type comes from a using or typedef statement).
  Usr alias_of = 0;

  auto cista_members() {
    return std::tie(static_cast<QueryEntityDef &>(*this), bases, funcs, types,
                    vars, alias_of);
  }
};

struct alignas(16) QueryVarDef : QueryEntityDef, NameMixin<QueryVarDef> {
  cista_types::vector<Usr> bases;
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

  auto cista_members() {
    return std::tie(static_cast<QueryEntityDef &>(*this), bases, type, storage);
  }
};

template <typename _EntityType, typename QDef>
struct QueryObject : QueryEntity {
  using EntityType = _EntityType;
  using Def = QDef;

  SubdbCursor<EntityID, Def> defCursor(const DB &db) const;
  SubdbCursor<EntityID, DeclRef> declCursor(const DB &db) const;
  SubdbCursor<EntityID, Use> useCursor(const DB &db) const;

  const Def *getFirstDef(const DB &db) const;
  const Def *getAnyDef(const DB &db) const;
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
  if constexpr (std::is_same_v<T, QueryFunc::Def>)
    return Kind::Func;
  else if constexpr (std::is_same_v<T, QueryType::Def>)
    return Kind::Type;
  else if constexpr (std::is_same_v<T, QueryVar::Def>)
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
  std::vector<std::pair<Usr, QueryFuncDef>> funcs_removed;
  std::vector<std::pair<Usr, QueryFuncDef>> funcs_def_update;
  Update<DeclRef> funcs_declarations;
  Update<Use> funcs_uses;
  Update<Usr> funcs_derived;

  // Type updates.
  int types_hint;
  std::vector<std::pair<Usr, QueryTypeDef>> types_removed;
  std::vector<std::pair<Usr, QueryTypeDef>> types_def_update;
  Update<DeclRef> types_declarations;
  Update<Use> types_uses;
  Update<Usr> types_derived;
  Update<Usr> types_instances;

  // Variable updates.
  int vars_hint;
  std::vector<std::pair<Usr, QueryVarDef>> vars_removed;
  std::vector<std::pair<Usr, QueryVarDef>> vars_def_update;
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
  template <typename, typename> friend struct SubdbCursor;
  friend int CompareDataInObjectsDBI(MDB_txn *txn, void *txn_ctx,
                                     const MDB_val *a, const MDB_val *b);

  lmdb::env env;

  lmdb::dbi dbi_files;
  lmdb::dbi dbi_name2file_id;

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

  const QueryFile &getFile(int file_id) const;
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

  template <typename Fn> bool allOfUsr(Kind kind, Fn &&fn) const;

  template <typename ET> const auto *entityGetFirstDef(ET &&entity) const;
  template <typename ET> const auto *entityGetAnyDef(ET &&entity) const;

private:
  int getFileId(std::string_view path);
  void putFile(int file_id, QueryFile &file);

  ObjId allocObjId(std::string_view data);
  void removeObj(ObjId obj_id);

  template <typename ET, typename Def> void insertEntityDef(ET &&e, Def &&def);

  template <typename CursorType> void removeEntityDef(CursorType &&it);

  template <Kind kind>
  std::pair<QueryEntityType<kind>, bool> idPutEntity(Usr usr);
  std::pair<QueryEntity, bool> idPutEntity(Kind kind, Usr usr);

  void idRemoveEntity(SubdbCursor<EntityID, QueryEntity> &it);

  template <typename Def>
  void removeUsrs(int file_id,
                  const std::vector<std::pair<Usr, Def>> &to_remove);

  int update(std::unordered_map<int, QueryFile> &, QueryFile::DefUpdate &&u);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, QueryFunc::Def>> &&us);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, QueryType::Def>> &&us);
  void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &,
              int file_id, std::vector<std::pair<Usr, QueryVar::Def>> &&us);

  friend struct QueryStore;
  friend struct QueryContainer;
  friend struct QueryFunc;
  friend struct QueryType;
  friend struct QueryVar;
  template <typename, typename> friend struct SubdbCursor;

  QSConnection qs;
  lmdb::txn &txn;
};

struct TxnDB {
  static TxnDB begin(QSConnection qs, bool read_only) {
    TxnDB Result;
    std::unique_ptr<lmdb::txn> txn = std::make_unique<lmdb::txn>(nullptr);
    std::unique_ptr<DB> dbptr = std::make_unique<DB>(qs, *txn);
    do {
      try {
        *txn = lmdb::txn::begin(qs->env, nullptr, read_only ? MDB_RDONLY : 0);
      } catch (const lmdb::runtime_error &e) {
        if (e.code() != MDB_MAP_RESIZED)
          throw;
        qs->env.set_mapsize(0);
      }
    } while (txn->handle() == nullptr);
    Result.qs = qs;
    Result.txn = std::move(txn);
    Result._db = std::move(dbptr);
    return Result;
  }

  void commit() {
    txn->commit();
    _db.reset();
  }

  void abort() noexcept {
    txn->abort();
    _db.reset();
  }

  DB *db() { return _db ? _db.get() : nullptr; }

private:
  QSConnection qs;
  std::unique_ptr<lmdb::txn> txn;
  std::unique_ptr<DB> _db;
};

template <typename _EntityType, typename QDef>
auto QueryObject<_EntityType, QDef>::defCursor(const DB &db) const
    -> SubdbCursor<EntityID, Def> {
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
const QDef *QueryObject<_EntityType, QDef>::getFirstDef(const DB &db) const {
  return db.entityGetFirstDef(static_cast<const EntityType &>(*this));
}

template <typename _EntityType, typename QDef>
const QDef *QueryObject<_EntityType, QDef>::getAnyDef(const DB &db) const {
  return db.entityGetAnyDef(static_cast<const EntityType &>(*this));
}

template <typename Fn> bool DB::allOfUsr(Kind kind, Fn &&fn) const {
  auto cursor = SubdbCursor<Kind, EntityID>::makeCursor(
      this, txn, qs->kind_to_entity_id, kind);
  CCLS_CURSOR_FOREACH(cursor, id) {
    if (!fn(id))
      return false;
  }
  return true;
}

template <typename ET> const auto *DB::entityGetFirstDef(ET &&entity) const {
  using _ET = typename std::remove_cv_t<std::remove_reference_t<ET>>;
  auto cursor = entity.defCursor(*this);
  const typename _ET::Def *def = cursor.first();
  return def;
}

template <typename ET> const auto *DB::entityGetAnyDef(ET &&entity) const {
  using _ET = typename std::remove_cv_t<std::remove_reference_t<ET>>;
  const typename _ET::Def *ret = nullptr;
  auto cursor = entity.defCursor(*this);
  allOf(entity.defCursor(*this), [&](auto &&def) {
    ret = &def;
    if (ret->spell)
      return false;
    return true;
  });
  return ret;
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
    allOf(entity.defCursor(*db), [&fn](auto &&def) {
      if (!fn(def))
        return false;
      return true;
    });
  });
}

template <typename Fn>
void eachOccurrence(DB *db, SymbolIdx sym, bool include_decl, Fn &&fn) {
  withEntity(db, sym, [&](const auto &entity) {
    allOf(entity.useCursor(*db), [&fn](auto &&use) {
      fn(use);
      return true;
    });
    if (include_decl) {
      allOf(entity.defCursor(*db), [&fn](auto &&def) {
        if (def.spell)
          fn(*def.spell);
        return true;
      });
      allOf(entity.declCursor(*db), [&fn](auto &&dr) {
        fn(dr);
        return true;
      });
    }
  });
}

SymbolKind getSymbolKind(DB *db, SymbolIdx sym);

template <typename C, typename Fn>
void eachDefinedFunc(DB *db, C &&usrs, Fn &&fn) {
  allOf(usrs, [&](auto usr) {
    const auto &obj = db->getFunc(usr);
    if (obj.defCursor(*db).count() != 0)
      fn(obj);
    return true;
  });
}

} // namespace ccls