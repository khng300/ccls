// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "indexer.hh"
#include "serializer.hh"
#include "working_files.hh"

#include "db_allocator.hh"
#include <boost/interprocess/containers/string.hpp>
#include <scoped_allocator>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>

#include <map>

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
namespace db {
//
// Begin of types
//
using Handle = impl::Handle;
template <typename T> using allocator = impl::allocator<T>;
template <typename T>
using scoped_allocator = std::scoped_allocator_adaptor<allocator<T>>;
inline allocator<void> getAlloc(Handle handle = {}) {
  return allocator<void>(handle);
}

template <typename K, typename V>
using scoped_map =
    std::map<K, V, std::less<void>,
             scoped_allocator<typename std::map<K, V>::value_type>>;
template <typename K, typename V>
using scoped_unordered_map =
    std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                       scoped_allocator<typename std::map<K, V>::value_type>>;
template <typename T> using scoped_vector = std::vector<T, scoped_allocator<T>>;
using string = boost::interprocess::basic_string<char, std::char_traits<char>,
                                                 allocator<char>>;

inline std::string toStdString(const string &s) {
  return std::string(s.begin(), s.end());
}

template <typename SV> inline string toInMemScopedString(SV &&s) {
  return string(s.begin(), s.end(), getAlloc());
}
} // namespace db
} // namespace ccls

namespace std {
template <> struct hash<ccls::db::string> {
  size_t operator()(const ccls::db::string &t) const _NOEXCEPT {
    return boost::container::hash_value(t);
  }
};
} // namespace std

namespace ccls {
template <typename String, typename II, template <typename...> typename V>
struct FileDef {
  using IndexInclude = II;

  String path;
  V<String> args;
  LanguageId language;
  // Includes in the file.
  V<IndexInclude> includes;
  // Parts of the file which are disabled.
  V<Range> skipped_ranges;
  // Used by |$ccls/reload|.
  V<String> dependencies;
  // modification time of the file recorded
  int64_t mtime = 0;
};

struct DBIndexInclude : IndexIncludeBase<db::string> {
  template <typename Alloc>
  DBIndexInclude(const Alloc &alloc)
      : DBIndexInclude(std::allocator_arg, alloc) {}
  template <typename Alloc>
  DBIndexInclude(std::allocator_arg_t, const Alloc &alloc)
      : IndexIncludeBase{{}, decltype(resolved_path)(alloc)} {}

  template <typename Alloc>
  DBIndexInclude(const DBIndexInclude &rhs, const Alloc &alloc)
      : DBIndexInclude(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  DBIndexInclude(std::allocator_arg_t, const Alloc &alloc,
                 const DBIndexInclude &rhs)
      : IndexIncludeBase{rhs.line,
                         decltype(resolved_path)(rhs.resolved_path, alloc)} {}

  template <typename Alloc>
  DBIndexInclude(DBIndexInclude &&rhs, const Alloc &alloc)
      : DBIndexInclude(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  DBIndexInclude(std::allocator_arg_t, const Alloc &alloc, DBIndexInclude &&rhs)
      : IndexIncludeBase{rhs.line, decltype(resolved_path)(
                                       std::move(rhs.resolved_path), alloc)} {}
};

struct DBFileDef : FileDef<db::string, DBIndexInclude, db::scoped_vector> {
  template <typename Alloc>
  DBFileDef(const Alloc &alloc) : DBFileDef(std::allocator_arg, alloc) {}
  template <typename Alloc>
  DBFileDef(std::allocator_arg_t, const Alloc &alloc)
      : FileDef{decltype(path)(alloc),
                decltype(args)(alloc),
                {},
                decltype(includes)(alloc),
                decltype(skipped_ranges)(alloc),
                decltype(dependencies)(alloc)} {}

  template <typename Alloc>
  DBFileDef(const DBFileDef &rhs, const Alloc &alloc)
      : DBFileDef(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  DBFileDef(std::allocator_arg_t, const Alloc &alloc, const DBFileDef &rhs)
      : FileDef{decltype(path)(rhs.path, alloc),
                decltype(args)(rhs.args, alloc),
                decltype(language)(rhs.language),
                decltype(includes)(rhs.includes, alloc),
                decltype(skipped_ranges)(rhs.skipped_ranges, alloc),
                decltype(dependencies)(rhs.dependencies, alloc)} {}

  template <typename Alloc>
  DBFileDef(DBFileDef &&rhs, const Alloc &alloc)
      : DBFileDef(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  DBFileDef(std::allocator_arg_t, const Alloc &alloc, DBFileDef &&rhs)
      : FileDef{decltype(path)(std::move(rhs.path), alloc),
                decltype(args)(std::move(rhs.args), alloc),
                decltype(language)(rhs.language),
                decltype(includes)(std::move(rhs.includes), alloc),
                decltype(skipped_ranges)(std::move(rhs.skipped_ranges), alloc),
                decltype(dependencies)(std::move(rhs.dependencies), alloc)} {}
};

struct QueryFile {
  using Def = DBFileDef;
  using CoreDef = FileDef<std::string, IndexInclude, std::vector>;
  using DefUpdate = std::pair<CoreDef, std::string>;

  int id = -1;
  std::optional<Def> def;
  // `extent` is valid => declaration; invalid => regular reference
  db::scoped_unordered_map<ExtentRef, int> symbol2refcnt;

  template <typename Alloc>
  QueryFile(std::allocator_arg_t, const Alloc &alloc, const QueryFile &rhs)
      : def(rhs.def), symbol2refcnt(rhs.symbol2refcnt, alloc) {}
  template <typename Alloc>
  QueryFile(const Alloc &alloc) : QueryFile(std::allocator_arg, alloc) {}

  template <typename Alloc>
  QueryFile(std::allocator_arg_t, const Alloc &alloc) : symbol2refcnt(alloc) {}
};

template <typename Q, typename QDef> struct QueryEntity {
  using Def = QDef;
  Def *anyDef() {
    Def *ret = nullptr;
    for (auto &i : static_cast<Q *>(this)->def) {
      ret = &i;
      if (i.spell)
        break;
    }
    return ret;
  }
  const Def *anyDef() const {
    return const_cast<QueryEntity *>(this)->anyDef();
  }
};

template <typename T>
using Update =
    std::unordered_map<Usr, std::pair<std::vector<T>, std::vector<T>>>;

struct DBFuncDef : FuncDef<db::scoped_vector, db::string> {
  template <typename Alloc>
  DBFuncDef(const Alloc &alloc) : DBFuncDef(std::allocator_arg, alloc) {}
  template <typename Alloc>
  DBFuncDef(std::allocator_arg_t, const Alloc &alloc)
      : FuncDef{{},
                decltype(detailed_name)(alloc),
                decltype(hover)(alloc),
                decltype(comments)(alloc),
                {},
                decltype(bases)(alloc),
                decltype(vars)(alloc),
                decltype(callees)(alloc)} {}

  template <typename Alloc>
  DBFuncDef(const DBFuncDef &rhs, const Alloc &alloc)
      : DBFuncDef(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  DBFuncDef(std::allocator_arg_t, const Alloc &alloc, const DBFuncDef &rhs)
      : FuncDef{{},
                decltype(detailed_name)(rhs.detailed_name, alloc),
                decltype(hover)(rhs.hover, alloc),
                decltype(comments)(rhs.comments, alloc),
                {},
                decltype(bases)(rhs.bases, alloc),
                decltype(vars)(rhs.vars, alloc),
                decltype(callees)(rhs.callees, alloc)} {
    spell = rhs.spell;
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
    storage = rhs.storage;
  }

  template <typename Alloc>
  DBFuncDef(DBFuncDef &&rhs, const Alloc &alloc)
      : DBFuncDef(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  DBFuncDef(std::allocator_arg_t, const Alloc &alloc, DBFuncDef &&rhs)
      : FuncDef{{},
                decltype(detailed_name)(std::move(rhs.detailed_name), alloc),
                decltype(hover)(std::move(rhs.hover), alloc),
                decltype(comments)(std::move(rhs.comments), alloc),
                decltype(spell)(std::move(rhs.spell)),
                decltype(bases)(std::move(rhs.bases), alloc),
                decltype(vars)(std::move(rhs.vars), alloc),
                decltype(callees)(std::move(rhs.callees), alloc)} {
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
    storage = rhs.storage;
  }
};
struct QueryFunc : QueryEntity<QueryFunc, DBFuncDef> {
  Usr usr;
  db::scoped_vector<Def> def;
  db::scoped_vector<DeclRef> declarations;
  db::scoped_vector<Usr> derived;
  db::scoped_vector<Use> uses;

  template <typename Alloc>
  QueryFunc(std::allocator_arg_t, const Alloc &alloc)
      : def(alloc), declarations(alloc), derived(alloc), uses(alloc) {}
  template <typename Alloc>
  QueryFunc(const Alloc &alloc) : QueryFunc(std::allocator_arg, alloc) {}

  template <typename Alloc>
  QueryFunc(const QueryFunc &rhs, const Alloc &alloc)
      : QueryFunc(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  QueryFunc(std::allocator_arg_t, const Alloc &alloc, const QueryFunc &rhs)
      : def(rhs.def, alloc), declarations(rhs.declarations, alloc),
        derived(rhs.derived, alloc), uses(rhs.uses, alloc) {
    usr = rhs.usr;
  }

  template <typename Alloc>
  QueryFunc(QueryFunc &&rhs, const Alloc &alloc)
      : QueryFunc(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  QueryFunc(std::allocator_arg_t, const Alloc &alloc, QueryFunc &&rhs)
      : def(std::move(rhs.def), alloc),
        declarations(std::move(rhs.declarations), alloc),
        derived(std::move(rhs.derived), alloc),
        uses(std::move(rhs.uses), alloc) {
    usr = rhs.usr;
  }
};

struct DBTypeDef : TypeDef<db::scoped_vector, db::string> {
  template <typename Alloc>
  DBTypeDef(const Alloc &alloc) : DBTypeDef(std::allocator_arg, alloc) {}
  template <typename Alloc>
  DBTypeDef(std::allocator_arg_t, const Alloc &alloc)
      : TypeDef{{},
                decltype(detailed_name)(alloc),
                decltype(hover)(alloc),
                decltype(comments)(alloc),
                {},
                decltype(bases)(alloc),
                decltype(funcs)(alloc),
                decltype(types)(alloc),
                decltype(vars)(alloc)} {}

  template <typename Alloc>
  DBTypeDef(const DBTypeDef &rhs, const Alloc &alloc)
      : DBTypeDef(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  DBTypeDef(std::allocator_arg_t, const Alloc &alloc, const DBTypeDef &rhs)
      : TypeDef{{},
                decltype(detailed_name)(rhs.detailed_name, alloc),
                decltype(hover)(rhs.hover, alloc),
                decltype(comments)(rhs.comments, alloc),
                decltype(spell)(rhs.spell),
                decltype(bases)(rhs.bases, alloc),
                decltype(funcs)(rhs.funcs, alloc),
                decltype(types)(rhs.types, alloc),
                decltype(vars)(rhs.vars, alloc)} {
    alias_of = rhs.alias_of;
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
  }

  template <typename Alloc>
  DBTypeDef(DBTypeDef &&rhs, const Alloc &alloc)
      : DBTypeDef(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  DBTypeDef(std::allocator_arg_t, const Alloc &alloc, DBTypeDef &&rhs)
      : TypeDef{{},
                decltype(detailed_name)(std::move(rhs.detailed_name), alloc),
                decltype(hover)(std::move(rhs.hover), alloc),
                decltype(comments)(std::move(rhs.comments), alloc),
                decltype(spell)(std::move(rhs.spell)),
                decltype(bases)(std::move(rhs.bases), alloc),
                decltype(funcs)(std::move(rhs.funcs), alloc),
                decltype(types)(std::move(rhs.types), alloc),
                decltype(vars)(std::move(rhs.vars), alloc)} {
    alias_of = rhs.alias_of;
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
  }
};
struct QueryType : QueryEntity<QueryType, DBTypeDef> {
  Usr usr;
  db::scoped_vector<Def> def;
  db::scoped_vector<DeclRef> declarations;
  db::scoped_vector<Usr> derived;
  db::scoped_vector<Usr> instances;
  db::scoped_vector<Use> uses;

  template <typename Alloc>
  QueryType(std::allocator_arg_t, const Alloc &alloc)
      : def(alloc), declarations(alloc), derived(alloc), instances(alloc),
        uses(alloc) {}
  template <typename Alloc>
  QueryType(const Alloc &alloc) : QueryType(std::allocator_arg, alloc) {}

  template <typename Alloc>
  QueryType(const QueryType &rhs, const Alloc &alloc)
      : QueryType(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  QueryType(std::allocator_arg_t, const Alloc &alloc, const QueryType &rhs)
      : def(rhs.def, alloc), declarations(rhs.declarations, alloc),
        derived(rhs.derived, alloc), instances(rhs.instances, alloc),
        uses(rhs.uses, alloc) {
    usr = rhs.usr;
  }

  template <typename Alloc>
  QueryType(QueryType &&rhs, const Alloc &alloc)
      : QueryType(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  QueryType(std::allocator_arg_t, const Alloc &alloc, QueryType &&rhs)
      : def(std::move(rhs.def), alloc),
        declarations(std::move(rhs.declarations), alloc),
        derived(std::move(rhs.derived), alloc),
        instances(std::move(rhs.instances), alloc),
        uses(std::move(rhs.uses), alloc) {
    usr = rhs.usr;
  }
};

struct DBVarDef : VarDef<db::string> {
  template <typename Alloc>
  DBVarDef(const Alloc &alloc) : DBVarDef(std::allocator_arg, alloc) {}
  template <typename Alloc>
  DBVarDef(std::allocator_arg_t, const Alloc &alloc)
      : VarDef{{},
               decltype(detailed_name)(alloc),
               decltype(hover)(alloc),
               decltype(comments)(alloc)} {}

  template <typename Alloc>
  DBVarDef(const DBVarDef &rhs, const Alloc &alloc)
      : DBVarDef(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  DBVarDef(std::allocator_arg_t, const Alloc &alloc, const DBVarDef &rhs)
      : VarDef{{},
               decltype(detailed_name)(rhs.detailed_name, alloc),
               decltype(hover)(rhs.hover, alloc),
               decltype(comments)(rhs.comments, alloc),
               decltype(spell)(rhs.spell)} {
    type = rhs.type;
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
    storage = rhs.storage;
  }

  template <typename Alloc>
  DBVarDef(DBVarDef &&rhs, const Alloc &alloc)
      : DBVarDef(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  DBVarDef(std::allocator_arg_t, const Alloc &alloc, DBVarDef &&rhs)
      : VarDef{{},
               decltype(detailed_name)(std::move(rhs.detailed_name), alloc),
               decltype(hover)(std::move(rhs.hover), alloc),
               decltype(comments)(std::move(rhs.comments), alloc),
               decltype(spell)(std::move(rhs.spell))} {
    type = rhs.type;
    file_id = rhs.file_id;
    qual_name_offset = rhs.qual_name_offset;
    short_name_offset = rhs.short_name_offset;
    short_name_size = rhs.short_name_size;
    kind = rhs.kind;
    parent_kind = rhs.parent_kind;
    storage = rhs.storage;
  }
};
struct QueryVar : QueryEntity<QueryVar, DBVarDef> {
  Usr usr;
  db::scoped_vector<Def> def;
  db::scoped_vector<DeclRef> declarations;
  db::scoped_vector<Use> uses;

  // QueryVar() : def(), declarations(), uses() {}
  template <typename Alloc>
  QueryVar(const Alloc &alloc) : QueryVar(std::allocator_arg, alloc) {}
  template <typename Alloc>
  QueryVar(std::allocator_arg_t, const Alloc &alloc)
      : def(alloc), declarations(alloc), uses(alloc) {}

  template <typename Alloc>
  QueryVar(const QueryVar &rhs, const Alloc &alloc)
      : QueryVar(std::allocator_arg, alloc, rhs) {}
  template <typename Alloc>
  QueryVar(std::allocator_arg_t, const Alloc &alloc, const QueryVar &rhs)
      : def(rhs.def, alloc), declarations(rhs.declarations, alloc),
        uses(rhs.uses, alloc) {
    usr = rhs.usr;
  }

  template <typename Alloc>
  QueryVar(QueryVar &&rhs, const Alloc &alloc)
      : QueryVar(std::allocator_arg, alloc, std::move(rhs)) {}
  template <typename Alloc>
  QueryVar(std::allocator_arg_t, const Alloc &alloc, QueryVar &&rhs)
      : def(std::move(rhs.def), alloc),
        declarations(std::move(rhs.declarations), alloc),
        uses(std::move(rhs.uses), alloc) {
    usr = rhs.usr;
  }
};

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
  std::vector<std::pair<Usr, QueryFunc::Def>> funcs_removed;
  std::vector<std::pair<Usr, QueryFunc::Def>> funcs_def_update;
  Update<DeclRef> funcs_declarations;
  Update<Use> funcs_uses;
  Update<Usr> funcs_derived;

  // Type updates.
  int types_hint;
  std::vector<std::pair<Usr, QueryType::Def>> types_removed;
  std::vector<std::pair<Usr, QueryType::Def>> types_def_update;
  Update<DeclRef> types_declarations;
  Update<Use> types_uses;
  Update<Usr> types_derived;
  Update<Usr> types_instances;

  // Variable updates.
  int vars_hint;
  std::vector<std::pair<Usr, QueryVar::Def>> vars_removed;
  std::vector<std::pair<Usr, QueryVar::Def>> vars_def_update;
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

// The query database is heavily optimized for fast queries. It is stored
// in-memory.
struct DB {
  db::scoped_map<int, QueryFile> files;
  db::scoped_unordered_map<db::string, int> name2file_id;
  db::scoped_map<Usr, std::size_t> func_usr, type_usr, var_usr;
  db::scoped_map<std::size_t, QueryFunc> funcs;
  db::scoped_map<std::size_t, QueryType> types;
  db::scoped_map<std::size_t, QueryVar> vars;
  db::allocator<DB> allocator;

  DB(const db::allocator<DB> &alloc) : DB(std::allocator_arg, alloc) {}
  DB(std::allocator_arg_t, const db::allocator<DB> &alloc)
      : files(alloc), name2file_id(alloc), func_usr(alloc), type_usr(alloc),
        var_usr(alloc), funcs(alloc), types(alloc), vars(alloc),
        allocator(alloc) {}
  void clear();

  void populateVFS(VFS *vfs) const;

  template <typename Def>
  void removeUsrs(Kind kind, int file_id,
                  const std::vector<std::pair<Usr, Def>> &to_remove);
  // Insert the contents of |update| into |db|.
  void applyIndexUpdate(IndexUpdate *update);
  int getFileId(const std::string &path);
  int update(QueryFile::DefUpdate &&u);
  void update(const Lid2file_id &, int file_id,
              std::vector<std::pair<Usr, QueryType::Def>> &&us);
  void update(const Lid2file_id &, int file_id,
              std::vector<std::pair<Usr, QueryFunc::Def>> &&us);
  void update(const Lid2file_id &, int file_id,
              std::vector<std::pair<Usr, QueryVar::Def>> &&us);
  std::string_view getSymbolName(SymbolIdx sym, bool qualified);
  std::vector<uint8_t> getFileSet(const std::vector<std::string> &folders);

  bool hasFunc(Usr usr) const { return func_usr.count(usr); }
  bool hasType(Usr usr) const { return type_usr.count(usr); }
  bool hasVar(Usr usr) const { return var_usr.count(usr); }

  QueryFunc &getFunc(Usr usr) { return funcs[func_usr[usr]]; }
  QueryType &getType(Usr usr) { return types[type_usr[usr]]; }
  QueryVar &getVar(Usr usr) { return vars[var_usr[usr]]; }

  QueryFile &getFile(SymbolIdx ref) { return files[ref.usr]; }
  QueryFunc &getFunc(SymbolIdx ref) { return getFunc(ref.usr); }
  QueryType &getType(SymbolIdx ref) { return getType(ref.usr); }
  QueryVar &getVar(SymbolIdx ref) { return getVar(ref.usr); }
};

Maybe<DeclRef> getDefinitionSpell(DB *db, SymbolIdx sym);

// Get defining declaration (if exists) or an arbitrary declaration (otherwise)
// for each id.
std::vector<Use> getFuncDeclarations(DB *, const db::scoped_vector<Usr> &);
std::vector<Use> getFuncDeclarations(DB *, const Vec<Usr> &);
std::vector<Use> getTypeDeclarations(DB *, const db::scoped_vector<Usr> &);
std::vector<DeclRef> getVarDeclarations(DB *, const db::scoped_vector<Usr> &,
                                        unsigned);

// Get non-defining declarations.
std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym);

std::vector<Use> getUsesForAllBases(DB *db, QueryFunc &root);
std::vector<Use> getUsesForAllDerived(DB *db, QueryFunc &root);
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
                                             QueryFile *file, Position &ls_pos,
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
    for (auto &def : entity.def)
      if (!fn(def))
        break;
  });
}

template <typename Fn>
void eachOccurrence(DB *db, SymbolIdx sym, bool include_decl, Fn &&fn) {
  withEntity(db, sym, [&](const auto &entity) {
    for (Use use : entity.uses)
      fn(use);
    if (include_decl) {
      for (auto &def : entity.def)
        if (def.spell)
          fn(*def.spell);
      for (Use use : entity.declarations)
        fn(use);
    }
  });
}

SymbolKind getSymbolKind(DB *db, SymbolIdx sym);

template <typename C, typename Fn>
void eachDefinedFunc(DB *db, const C &usrs, Fn &&fn) {
  for (Usr usr : usrs) {
    auto &obj = db->getFunc(usr);
    if (!obj.def.empty())
      fn(obj);
  }
}
} // namespace ccls

namespace std {
template <typename Alloc>
struct uses_allocator<ccls::DBIndexInclude, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::DBFileDef, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::DBFuncDef, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::DBTypeDef, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::DBVarDef, Alloc> : std::true_type {};

template <typename Alloc>
struct uses_allocator<ccls::QueryFile, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::QueryFunc, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::QueryType, Alloc> : std::true_type {};
template <typename Alloc>
struct uses_allocator<ccls::QueryVar, Alloc> : std::true_type {};

template <typename Alloc>
struct uses_allocator<ccls::DB, Alloc> : std::true_type {};
} // namespace std
