// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "indexer.hh"
#include "lmdb_wrappers.hh"
#include "working_files.hh"

#include <cista/serialization.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>

#include <array>
#include <iterator>
#include <type_traits>

namespace llvm {
template <> struct DenseMapInfo<ccls::ExtentRef> {
  static inline ccls::ExtentRef getEmptyKey() { return {}; }
  static inline ccls::ExtentRef getTombstoneKey() { return {{ccls::Range(), ccls::Usr(-1)}}; }
  static unsigned getHashValue(ccls::ExtentRef sym) { return std::hash<ccls::ExtentRef>()(sym); }
  static bool isEqual(ccls::ExtentRef l, ccls::ExtentRef r) { return l == r; }
};
} // namespace llvm

namespace ccls {

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

struct alignas(uint64_t) EntityID {
  uint64_t hash;

  EntityID() = default;
  EntityID(Kind kind, Usr usr);
};

struct alignas(16) QueryEntity {
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
    return std::tie(detailed_name, hover, comments, static_cast<Ref &>(spell.storage), spell.storage.file_id,
                    spell.storage.extent, file_id, qual_name_offset, short_name_offset, short_name_size, kind,
                    parent_kind);
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

  auto cista_members() { return std::tie(static_cast<QueryEntityDef &>(*this), bases, vars, callees, storage); }
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

  auto cista_members() { return std::tie(static_cast<QueryEntityDef &>(*this), bases, funcs, types, vars, alias_of); }
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
           (parent_kind == SymbolKind::Function || parent_kind == SymbolKind::Method ||
            parent_kind == SymbolKind::StaticMethod || parent_kind == SymbolKind::Constructor) &&
           (storage == clang::SC_None || storage == clang::SC_Auto || storage == clang::SC_Register);
  }

  auto cista_members() { return std::tie(static_cast<QueryEntityDef &>(*this), bases, type, storage); }
};

template <typename QType, typename QDef> class QueryObject : public QueryEntity {
public:
  using Type = QType;
  using Def = QDef;

  QueryObject(const DB &db, const QueryEntity &e) : QueryEntity(e), db_(&db) {}

  SubdbContainer<EntityID, Def> defs() const;
  SubdbContainer<EntityID, DeclRef> decls() const;
  SubdbContainer<EntityID, Use> uses() const;

  const Def *anyDef() const;

protected:
  const DB *db_;
};

class QueryFunc : public QueryObject<QueryFunc, QueryFuncDef> {
public:
  QueryFunc(const DB &db, const QueryEntity &e) : QueryObject(db, e) {}

  inline SubdbContainer<EntityID, Usr> deriveds() const;
};

class QueryType : public QueryObject<QueryType, QueryTypeDef> {
public:
  QueryType(const DB &db, const QueryEntity &e) : QueryObject(db, e) {}

  inline SubdbContainer<EntityID, Usr> deriveds() const;
  inline SubdbContainer<EntityID, Usr> instances() const;
};

class QueryVar : public QueryObject<QueryVar, QueryVarDef> {
public:
  QueryVar(const DB &db, const QueryEntity &e) : QueryObject(db, e) {}
};

template <typename T> using Update = std::unordered_map<Usr, std::pair<std::vector<T>, std::vector<T>>>;

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
  friend class DB;
  friend class TxnManager;

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

class DB {
  template <typename, typename> friend class QueryObject;
  friend class QueryFunc;
  friend class QueryType;
  friend class QueryVar;

public:
  DB(QueryStoreConnection qs, lmdb::txn &txn) : qs_(qs), txn_(txn) {}

  void clear();
  // Insert the contents of |update| into |db|.
  void applyIndexUpdate(IndexUpdate *update);

  void populateVFS(VFS *vfs) const;

  std::optional<int> findFileId(const std::string &path) const;
  std::string getSymbolName(SymbolIdx sym, bool qualified) const;
  std::vector<uint8_t> getFileSet(const std::vector<std::string> &folders) const;

  bool hasEntityId(Kind kind, Usr usr) const;
  bool hasFunc(Usr usr) const { return hasEntityId(Kind::Func, usr); }
  bool hasType(Usr usr) const { return hasEntityId(Kind::Type, usr); }
  bool hasVar(Usr usr) const { return hasEntityId(Kind::Var, usr); }

  const QueryEntity &id2Entity(EntityID id) const;
  QueryFunc id2Func(EntityID id) const { return QueryFunc(*this, id2Entity(id)); }
  QueryType id2Type(EntityID id) const { return QueryType(*this, id2Entity(id)); }
  QueryVar id2Var(EntityID id) const { return QueryVar(*this, id2Entity(id)); }

  QueryFunc getFunc(Usr usr) const { return id2Func(EntityID(Kind::Func, usr)); }
  QueryType getType(Usr usr) const { return id2Type(EntityID(Kind::Type, usr)); }
  QueryVar getVar(Usr usr) const { return id2Var(EntityID(Kind::Var, usr)); }

  const QueryFile &getFile(int file_id) const;
  QueryFunc getFunc(SymbolIdx ref) const { return getFunc(ref.usr); }
  QueryType getType(SymbolIdx ref) const { return getType(ref.usr); }
  QueryVar getVar(SymbolIdx ref) const { return getVar(ref.usr); }

  DbContainer<int, QueryFile> files() const;
  SubdbContainer<Kind, EntityID> allUsrs(Kind kind) const;

private:
  struct Updater {
    Updater(DB &db) : db_(db), qs_(db.qs_), txn_(db.txn_) {}

    void applyIndexUpdate(IndexUpdate *update);

  private:
    int getFileId(std::string_view path);
    void putFile(int file_id, QueryFile &file);

    template <typename ET, typename Def> void insertEntityDef(ET &&e, Def &&def);
    std::pair<QueryEntity, bool> idPutEntity(Kind kind, Usr usr);
    void idRemoveEntity(SubdbIterator<EntityID, QueryEntity> &&);

    int update(std::unordered_map<int, QueryFile> &, QueryFile::DefUpdate &&u);
    void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &, int file_id,
                std::vector<std::pair<Usr, QueryFunc::Def>> &&us);
    void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &, int file_id,
                std::vector<std::pair<Usr, QueryType::Def>> &&us);
    void update(std::unordered_map<int, QueryFile> &, const Lid2file_id &, int file_id,
                std::vector<std::pair<Usr, QueryVar::Def>> &&us);

    template <typename Def> void removeUsrs(int file_id, const std::vector<std::pair<Usr, Def>> &to_remove);

    DB &db_;
    QueryStoreConnection qs_;
    lmdb::txn &txn_;
  };

  SubdbContainer<EntityID, QueryEntityDef> entityDefs(const QueryEntity &entity) const;
  SubdbContainer<EntityID, DeclRef> entityDecls(const QueryEntity &entity) const;
  SubdbContainer<EntityID, Usr> entityDeriveds(const QueryEntity &entity) const;
  SubdbContainer<EntityID, Usr> entityInstances(const QueryEntity &entity) const;
  SubdbContainer<EntityID, Use> entityUses(const QueryEntity &entity) const;

  QueryStoreConnection qs_;
  lmdb::txn &txn_;
};

class TxnManager {
public:
  static TxnManager begin(QueryStoreConnection qs, bool read_only);

  void commit() &&;
  void abort() && noexcept;

  DB *db() { return &*db_; }
  lmdb::txn &txn() { return *txn_; }

private:
  QueryStoreConnection qs_;
  std::unique_ptr<lmdb::txn> txn_;
  std::unique_ptr<DB> db_;
};

template <typename QType, typename QDef> auto QueryObject<QType, QDef>::defs() const -> SubdbContainer<EntityID, Def> {
  return db_->entityDefs(*this);
}
template <typename QType, typename QDef> SubdbContainer<EntityID, DeclRef> QueryObject<QType, QDef>::decls() const {
  return db_->entityDecls(*this);
}
template <typename QType, typename QDef> SubdbContainer<EntityID, Use> QueryObject<QType, QDef>::uses() const {
  return db_->entityUses(*this);
}
inline SubdbContainer<EntityID, Usr> QueryFunc::deriveds() const { return (db_->entityDeriveds(*this)); }
inline SubdbContainer<EntityID, Usr> QueryType::deriveds() const { return (db_->entityDeriveds(*this)); }
inline SubdbContainer<EntityID, Usr> QueryType::instances() const { return (db_->entityInstances(*this)); }

template <typename QType, typename QDef> const QDef *QueryObject<QType, QDef>::anyDef() const {
  const Def *ret = nullptr;
  allOf(defs(), [&](auto &&def) {
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

std::vector<Use> getFuncDeclarations(DB *, SubdbContainer<EntityID, Usr> &);
std::vector<Use> getTypeDeclarations(DB *, SubdbContainer<EntityID, Usr> &);
std::vector<DeclRef> getVarDeclarations(DB *, SubdbContainer<EntityID, Usr> &, unsigned);
inline std::vector<Use> getFuncDeclarations(DB *db, SubdbContainer<EntityID, Usr> &&decls) {
  return getFuncDeclarations(db, decls);
}
inline std::vector<Use> getTypeDeclarations(DB *db, SubdbContainer<EntityID, Usr> &&decls) {
  return getTypeDeclarations(db, decls);
}
inline std::vector<DeclRef> getVarDeclarations(DB *db, SubdbContainer<EntityID, Usr> &&decls, unsigned kind) {
  return getVarDeclarations(db, decls, kind);
}

// Get non-defining declarations.
std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym);

std::vector<Use> getUsesForAllBases(DB *db, const QueryFunc &root);
std::vector<Use> getUsesForAllDerived(DB *db, const QueryFunc &root);
std::optional<lsRange> getLsRange(WorkingFile *working_file, const Range &location);
DocumentUri getLsDocumentUri(DB *db, int file_id, std::string *path);
DocumentUri getLsDocumentUri(DB *db, int file_id);

std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles, Use use);
std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles, SymbolRef sym, int file_id);
LocationLink getLocationLink(DB *db, WorkingFiles *wfiles, DeclRef dr);

// Returns a symbol. The symbol will *NOT* have a location assigned.
std::optional<SymbolInformation> getSymbolInfo(DB *db, SymbolIdx sym, bool detailed);

std::vector<SymbolRef> findSymbolsAtLocation(WorkingFile *working_file, const QueryFile *file, Position &ls_pos,
                                             bool smallest = false);

template <typename ContainerType, typename Fn> bool allOf(ContainerType &&container, Fn &&fn) {
  for (auto it = std::begin(container), end_it = std::end(container); it != end_it; ++it) {
    if (!fn(*it))
      return false;
  }
  return true;
}

template <typename ContainerType, typename Fn> void forEach(ContainerType &&container, Fn &&fn) {
  allOf(std::forward<ContainerType>(container), [&fn](auto &&it) {
    fn(std::forward<decltype(it)>(it));
    return true;
  });
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
    allOf(entity.defs(), [&fn](auto &&def) {
      if (!fn(def))
        return false;
      return true;
    });
  });
}

template <typename Fn> void eachOccurrence(DB *db, SymbolIdx sym, bool include_decl, Fn &&fn) {
  withEntity(db, sym, [&](const auto &entity) {
    forEach(entity.uses(), [&fn](auto &&use) { fn(use); });
    if (include_decl) {
      forEach(entity.defs(), [&fn](auto &&def) {
        if (def.spell)
          fn(*def.spell);
      });
      forEach(entity.decls(), [&fn](auto &&dr) { fn(dr); });
    }
  });
}

SymbolKind getSymbolKind(DB *db, SymbolIdx sym);

template <typename C, typename Fn> void eachDefinedFunc(DB *db, C &&usrs, Fn &&fn) {
  forEach(usrs, [&](auto usr) {
    const auto &obj = db->getFunc(usr);
    if (obj.defs().size() != 0)
      fn(obj);
  });
}

} // namespace ccls