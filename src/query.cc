// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "query.hh"
#include "log.hh"
#include "pipeline.hh"

#include <llvm/ADT/ScopeExit.h>

#include <rapidjson/document.h>
#include <siphash.h>

#include <llvm/ADT/STLExtras.h>

#include <assert.h>
#include <functional>
#include <limits.h>
#include <optional>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ccls {
template <typename K, typename V>
void reflect(BinaryReader &vis, std::unordered_map<K, V> &v) {
  size_t size;
  reflect(vis, size);
  for (size_t i = 0; i < size; i++) {
    K first;
    reflect(vis, first);
    reflect(vis, v[std::move(first)]);
  }
}
template <typename K, typename V>
void reflect(BinaryWriter &vis, std::unordered_map<K, V> &v) {
  size_t size = v.size();
  reflect(vis, size);
  for (auto &p : v) {
    reflect(vis, const_cast<K &>(p.first));
    reflect(vis, p.second);
  }
}

template <typename Vis> void reflect(Vis &vis, ExtentRef &v) {
  reflect(vis, static_cast<SymbolRef &>(v));
  reflect(vis, v.extent);
}

REFLECT_STRUCT(FileDef::IndexInclude, line, resolved_path);
REFLECT_UNDERLYING_B(LanguageId);
REFLECT_STRUCT(FileDef, path, args, language, dependencies, includes,
               skipped_ranges, mtime);
REFLECT_STRUCT(QueryFile, id, def, symbol2refcnt);

namespace {
std::string fileToStr(QueryFile &m) {
  BinaryWriter writer;
  reflect(writer, m);
  return writer.take();
}
std::string fileToStr(QueryFile &&m) { return fileToStr(m); }

QueryFile strToFile(std::string_view buf) {
  BinaryReader reader(buf);
  QueryFile result;
  reflect(reader, result);
  return result;
}

void assignFileId(const Lid2file_id &lid2file_id, int file_id, Use &use) {
  if (use.file_id == -1)
    use.file_id = file_id;
  else
    use.file_id = lid2file_id.find(use.file_id)->second;
}

FileDef::IndexInclude convert(const IndexInclude &o) {
  return {o.line, o.resolved_path};
}

QueryFile::DefUpdate buildFileDefUpdate(IndexFile &&indexed) {
  FileDef def;
  def.path = std::move(indexed.path);
  for (auto s : indexed.args)
    def.args.emplace_back(s);
  for (auto &m : indexed.includes)
    def.includes.emplace_back(convert(m));
  def.skipped_ranges = std::move(indexed.skipped_ranges);
  def.dependencies.reserve(indexed.dependencies.size());
  for (auto &dep : indexed.dependencies)
    def.dependencies.push_back(dep.first.val().data()); // llvm 8 -> data()
  def.language = indexed.language;
  def.mtime = indexed.mtime;
  return {std::move(def), std::move(indexed.file_contents)};
}
} // namespace

template <typename T> IUVector<T> convert(const std::vector<T> &o) {
  return {o.begin(), o.end()};
}

IUFuncDef convert(const IndexFunc::Def &o) {
  IUFuncDef r;
  r.detailed_name = o.detailed_name;
  r.hover = o.hover;
  r.comments = o.comments;
  r.spell = o.spell;
  r.bases = convert(o.bases);
  r.vars = convert(o.vars);
  r.callees = convert(o.callees);
  // no file_id
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  r.storage = o.storage;
  return r;
}

IUTypeDef convert(const IndexType::Def &o) {
  IUTypeDef r;
  r.detailed_name = o.detailed_name;
  r.hover = o.hover;
  r.comments = o.comments;
  r.spell = o.spell;
  r.bases = convert(o.bases);
  r.funcs = convert(o.funcs);
  r.types = convert(o.types);
  r.vars = convert(o.vars);
  r.alias_of = o.alias_of;
  // no file_id
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  return r;
}

IUVarDef convert(const IndexVar::Def &o) {
  IUVarDef r;
  r.detailed_name = o.detailed_name;
  r.hover = o.hover;
  r.comments = o.comments;
  r.spell = o.spell;
  r.type = o.type;
  // no file_id
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  return r;
}

QueryFunc::Def DB::allocate(const IUFuncDef &o,
                            const QueryFunc::Def &existing) {
  QueryFunc::Def r = existing;
  if (r.detailed_name.obj_id == InvalidObjId)
    r.detailed_name.obj_id = allocObjId();
  if (r.hover.obj_id == InvalidObjId)
    r.hover.obj_id = allocObjId();
  if (r.comments.obj_id == InvalidObjId)
    r.comments.obj_id = allocObjId();
  if (r.bases.obj_id == InvalidObjId)
    r.bases.obj_id = allocObjId();
  if (r.vars.obj_id == InvalidObjId)
    r.vars.obj_id = allocObjId();
  if (r.callees.obj_id == InvalidObjId)
    r.callees.obj_id = allocObjId();

  r.detailed_name.put(this, o.detailed_name);
  r.hover.put(this, o.hover);
  r.comments.put(this, o.comments);
  r.spell = o.spell;
  r.bases.put(this, llvm::ArrayRef<Usr>(o.bases));
  r.vars.put(this, llvm::ArrayRef<Usr>(o.vars));
  r.callees.put(this, llvm::ArrayRef<SymbolRef>(o.callees));
  r.file_id = o.file_id;
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  r.storage = o.storage;
  return r;
}

QueryType::Def DB::allocate(const IUTypeDef &o,
                            const QueryType::Def &existing) {
  QueryType::Def r = existing;
  if (r.detailed_name.obj_id == InvalidObjId)
    r.detailed_name.obj_id = allocObjId();
  if (r.hover.obj_id == InvalidObjId)
    r.hover.obj_id = allocObjId();
  if (r.comments.obj_id == InvalidObjId)
    r.comments.obj_id = allocObjId();
  if (r.bases.obj_id == InvalidObjId)
    r.bases.obj_id = allocObjId();
  if (r.funcs.obj_id == InvalidObjId)
    r.funcs.obj_id = allocObjId();
  if (r.types.obj_id == InvalidObjId)
    r.types.obj_id = allocObjId();
  if (r.vars.obj_id == InvalidObjId)
    r.vars.obj_id = allocObjId();

  r.detailed_name.put(this, o.detailed_name);
  r.hover.put(this, o.hover);
  r.comments.put(this, o.comments);
  r.spell = o.spell;
  r.bases.put(this, llvm::ArrayRef<Usr>(o.bases));
  r.funcs.put(this, llvm::ArrayRef<Usr>(o.funcs));
  r.types.put(this, llvm::ArrayRef<Usr>(o.types));
  r.vars.put(this, llvm::ArrayRef<std::pair<Usr, int64_t>>(o.vars));
  r.alias_of = o.alias_of;
  r.file_id = o.file_id;
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  return r;
}

QueryVar::Def DB::allocate(const IUVarDef &o, const QueryVar::Def &existing) {
  QueryVar::Def r = existing;
  if (r.detailed_name.obj_id == InvalidObjId)
    r.detailed_name.obj_id = allocObjId();
  if (r.hover.obj_id == InvalidObjId)
    r.hover.obj_id = allocObjId();
  if (r.comments.obj_id == InvalidObjId)
    r.comments.obj_id = allocObjId();
  if (r.bases.obj_id == InvalidObjId)
    r.bases.obj_id = allocObjId();

  r.detailed_name.put(this, o.detailed_name);
  r.hover.put(this, o.hover);
  r.comments.put(this, o.comments);
  r.spell = o.spell;
  r.type = o.type;
  r.file_id = o.file_id;
  r.qual_name_offset = o.qual_name_offset;
  r.short_name_offset = o.short_name_offset;
  r.short_name_size = o.short_name_size;
  r.kind = o.kind;
  r.parent_kind = o.parent_kind;
  return r;
}

std::string_view
QueryContainer::get(const DB *db) const {
  std::string_view key = lmdb::to_sv(obj_id), val;
  if (!db->qs->dbi_entities_objects.get(db->txn, key, val))
    return {};
  return val;
}

void QueryContainer::put(DB *db, std::string_view val) {
  std::string_view key = lmdb::to_sv(obj_id);
  db->qs->dbi_entities_objects.put(db->txn, key, val);
}

IndexUpdate IndexUpdate::createDelta(IndexFile *previous, IndexFile *current) {
  IndexUpdate r;
  static IndexFile empty(current->path, "<empty>", false);
  if (previous)
    r.prev_lid2path = std::move(previous->lid2path);
  else
    previous = &empty;
  r.lid2path = std::move(current->lid2path);

  r.funcs_hint = int(current->usr2func.size() - previous->usr2func.size());
  for (auto &it : previous->usr2func) {
    auto &func = it.second;
    if (!std::string_view(func.def.detailed_name).empty())
      r.funcs_removed.emplace_back(func.usr, convert(func.def));
    r.funcs_declarations[func.usr].first = std::move(func.declarations);
    r.funcs_uses[func.usr].first = std::move(func.uses);
    r.funcs_derived[func.usr].first = std::move(func.derived);
  }
  for (auto &it : current->usr2func) {
    auto &func = it.second;
    if (!std::string_view(func.def.detailed_name).empty())
      r.funcs_def_update.emplace_back(it.first, convert(func.def));
    r.funcs_declarations[func.usr].second = std::move(func.declarations);
    r.funcs_uses[func.usr].second = std::move(func.uses);
    r.funcs_derived[func.usr].second = std::move(func.derived);
  }

  r.types_hint = int(current->usr2type.size() - previous->usr2type.size());
  for (auto &it : previous->usr2type) {
    auto &type = it.second;
    if (!std::string_view(type.def.detailed_name).empty())
      r.types_removed.emplace_back(type.usr, convert(type.def));
    r.types_declarations[type.usr].first = std::move(type.declarations);
    r.types_uses[type.usr].first = std::move(type.uses);
    r.types_derived[type.usr].first = std::move(type.derived);
    r.types_instances[type.usr].first = std::move(type.instances);
  };
  for (auto &it : current->usr2type) {
    auto &type = it.second;
    if (!std::string_view(type.def.detailed_name).empty())
      r.types_def_update.emplace_back(it.first, convert(type.def));
    r.types_declarations[type.usr].second = std::move(type.declarations);
    r.types_uses[type.usr].second = std::move(type.uses);
    r.types_derived[type.usr].second = std::move(type.derived);
    r.types_instances[type.usr].second = std::move(type.instances);
  };

  r.vars_hint = int(current->usr2var.size() - previous->usr2var.size());
  for (auto &it : previous->usr2var) {
    auto &var = it.second;
    if (!std::string_view(var.def.detailed_name).empty())
      r.vars_removed.emplace_back(var.usr, convert(var.def));
    r.vars_declarations[var.usr].first = std::move(var.declarations);
    r.vars_uses[var.usr].first = std::move(var.uses);
  }
  for (auto &it : current->usr2var) {
    auto &var = it.second;
    if (!std::string_view(var.def.detailed_name).empty())
      r.vars_def_update.emplace_back(it.first, convert(var.def));
    r.vars_declarations[var.usr].second = std::move(var.declarations);
    r.vars_uses[var.usr].second = std::move(var.uses);
  }

  r.files_def_update = buildFileDefUpdate(std::move(*current));
  return r;
}

SubdbCursor<EntityID, Usr> QueryFunc::derivedCursor(const DB &db) const {
  return (db.entityDerivedCursor(*this));
}

SubdbCursor<EntityID, Usr> QueryType::derivedCursor(const DB &db) const {
  return (db.entityDerivedCursor(*this));
}

SubdbCursor<EntityID, Usr> QueryType::instanceCursor(const DB &db) const {
  return (db.entityInstanceCursor(*this));
}

QueryStore::QueryStore(std::string_view store_path)
    : env(std::move(env.create().set_max_dbs(48).open(store_path.data(),
                                                      MDB_NOMETASYNC))) {
  lmdb::txn txn(lmdb::txn::begin(env));
  dbi_entities_objects =
      lmdb::dbi::open(txn, "objects", MDB_INTEGERKEY | MDB_CREATE);
  dbi_entities_objects_freelist =
      lmdb::dbi::open(txn, "objects_freelist", MDB_INTEGERKEY | MDB_CREATE);
  dbi_files = lmdb::dbi::open(txn, "files", MDB_INTEGERKEY | MDB_CREATE);
  dbi_hashed_name2file_id =
      lmdb::dbi::open(txn, "hashed_name2file_id", MDB_CREATE);

  entities = lmdb::dbi::open(txn, "dbi_entities", MDB_CREATE);
  entities_defs =
      lmdb::dbi::open(txn, "dbi_entities_def", MDB_DUPSORT | MDB_CREATE);
  entities_declarations =
      lmdb::dbi::open(txn, "dbi_entities_declarations",
                      MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);
  entities_derived = lmdb::dbi::open(txn, "dbi_entities_derived",
                                     MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);
  entities_instances = lmdb::dbi::open(txn, "dbi_entities_instances",
                                       MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);
  entities_uses = lmdb::dbi::open(txn, "dbi_entities_uses",
                                  MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);
  kind_to_entity_id = lmdb::dbi::open(txn, "kind_to_entity_id",
                                      MDB_DUPSORT | MDB_DUPFIXED | MDB_CREATE);

  txn.commit();
}

void QueryStore::increaseMapSize() {
  MDB_envinfo envinfo;
  lmdb::env_info(env, &envinfo);
  mdb_size_t increment =
      std::max(envinfo.me_mapsize, mdb_size_t(128) * 1024 * 1024);
  env.set_mapsize(envinfo.me_mapsize + increment);
}

void DB::clear() {
  qs->dbi_entities_objects.drop(txn);
  qs->dbi_entities_objects_freelist.drop(txn);
  qs->dbi_files.drop(txn);
  qs->dbi_hashed_name2file_id.drop(txn);
  qs->entities.drop(txn);
  qs->entities_defs.drop(txn);
  qs->entities_declarations.drop(txn);
  qs->entities_derived.drop(txn);
  qs->entities_instances.drop(txn);
  qs->entities_uses.drop(txn);
  qs->kind_to_entity_id.drop(txn);
}

template <typename Def>
void DB::removeUsrs(int file_id,
                    const std::vector<std::pair<Usr, Def>> &to_remove) {
  constexpr Kind kind = DefToKind<Def>();
  for (auto &[usr, _] : to_remove) {
    if (!hasEntityId(kind, usr))
      continue;
    const auto &entity =
        id2Entity<QueryEntityType<kind>>(makeEntityID(kind, usr));
    auto cursor = entity.defCursor(*this);
    CCLS_CURSOR_FOREACH(cursor, def) {
      if (def.file_id == file_id) {
        removeEntityDef(cursor);
        break;
      }
    }
  }
}

void DB::applyIndexUpdate(IndexUpdate *u) {
#define REMOVE_ADD(Kind, C, F)                                                 \
  for (auto &it : u->C##s_##F) {                                               \
    auto usr = it.first;                                                       \
    auto e = idPutEntity(Kind, usr).first;                                     \
    for (auto &v : it.second.first)                                            \
      qs->entities_##F.del(txn, lmdb::to_sv(e.id), lmdb::to_sv(v));            \
    for (auto &v : it.second.second)                                           \
      qs->entities_##F.put(txn, lmdb::to_sv(e.id), lmdb::to_sv(v));            \
  }

  std::unordered_map<int, QueryFile> files;
  std::unordered_map<int, int> prev_lid2file_id, lid2file_id;
  for (auto &[lid, path] : u->prev_lid2path)
    prev_lid2file_id[lid] = getFileId(path);
  for (auto &[lid, path] : u->lid2path) {
    int file_id = getFileId(path);
    auto [it, inserted] = files.try_emplace(file_id);
    auto &file = it->second;
    if (inserted)
      file = getFile(file_id);
    lid2file_id[lid] = file_id;
    if (!file.def) {
      file.def.emplace();
      file.def->path = path;
    }
  }

  // References (Use &use) in this function are important to update file_id.
  auto ref = [&](std::unordered_map<int, int> &lid2fid, Usr usr, Kind kind,
                 Use &use, int delta) {
    use.file_id =
        use.file_id == -1 ? u->file_id : lid2fid.find(use.file_id)->second;
    ExtentRef sym{{use.range, usr, kind, use.role}};
    int &v = files[use.file_id].symbol2refcnt[sym];
    v += delta;
    assert(v >= 0);
    if (!v)
      files[use.file_id].symbol2refcnt.erase(sym);
  };
  auto refDecl = [&](std::unordered_map<int, int> &lid2fid, Usr usr, Kind kind,
                     DeclRef &dr, int delta) {
    dr.file_id =
        dr.file_id == -1 ? u->file_id : lid2fid.find(dr.file_id)->second;
    ExtentRef sym{{dr.range, usr, kind, dr.role}, dr.extent};
    int &v = files[dr.file_id].symbol2refcnt[sym];
    v += delta;
    assert(v >= 0);
    if (!v)
      files[dr.file_id].symbol2refcnt.erase(sym);
  };

  auto updateUses = [&](Usr usr, Kind kind, auto &p, bool hint_implicit,
                        lmdb::dbi &dbi_entities_uses) {
    auto [entity, _] = idPutEntity(kind, usr);
    for (Use &use : p.first) {
      if (hint_implicit && use.role & Role::Implicit) {
        // Make ranges of implicit function calls larger (spanning one more
        // column to the left/right). This is hacky but useful. e.g.
        // textDocument/definition on the space/semicolon in `A a;` or `
        // 42;` will take you to the constructor.
        if (use.range.start.column > 0)
          use.range.start.column--;
        use.range.end.column++;
      }
      ref(prev_lid2file_id, usr, kind, use, -1);
    }
    for (Use &use : p.first)
      dbi_entities_uses.del(txn, lmdb::to_sv(entity.id), lmdb::to_sv(use));
    for (Use &use : p.second) {
      if (hint_implicit && use.role & Role::Implicit) {
        if (use.range.start.column > 0)
          use.range.start.column--;
        use.range.end.column++;
      }
      ref(lid2file_id, usr, kind, use, 1);
    }
    for (Use &use : p.second)
      dbi_entities_uses.put(txn, lmdb::to_sv(entity.id), lmdb::to_sv(use));
  };

  if (u->files_removed) {
    auto hashed_name = hashUsr(lowerPathIfInsensitive(*u->files_removed));
    std::string_view val;
    if (qs->dbi_hashed_name2file_id.get(txn, lmdb::to_sv(hashed_name), val)) {
      int file_id = lmdb::from_sv<int>(val);
      files[file_id].def.reset();
    }
  }
  u->file_id =
      u->files_def_update ? update(files, std::move(*u->files_def_update)) : -1;

  for (auto &[usr, def] : u->funcs_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Func, *def.spell, -1);
  removeUsrs(u->file_id, u->funcs_removed);
  update(files, lid2file_id, u->file_id, std::move(u->funcs_def_update));
  for (auto &[usr, del_add] : u->funcs_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Func, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Func, dr, 1);
  }
  REMOVE_ADD(Kind::Func, func, declarations);
  REMOVE_ADD(Kind::Func, func, derived);
  for (auto &[usr, p] : u->funcs_uses)
    updateUses(usr, Kind::Func, p, true, qs->entities_uses);

  for (auto &[usr, def] : u->types_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Type, *def.spell, -1);
  removeUsrs(u->file_id, u->types_removed);
  update(files, lid2file_id, u->file_id, std::move(u->types_def_update));
  for (auto &[usr, del_add] : u->types_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Type, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Type, dr, 1);
  }
  REMOVE_ADD(Kind::Type, type, declarations);
  REMOVE_ADD(Kind::Type, type, derived);
  REMOVE_ADD(Kind::Type, type, instances);
  for (auto &[usr, p] : u->types_uses)
    updateUses(usr, Kind::Type, p, false, qs->entities_uses);

  for (auto &[usr, def] : u->vars_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Var, *def.spell, -1);
  removeUsrs(u->file_id, u->vars_removed);
  update(files, lid2file_id, u->file_id, std::move(u->vars_def_update));
  for (auto &[usr, del_add] : u->vars_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Var, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Var, dr, 1);
  }
  REMOVE_ADD(Kind::Var, var, declarations);
  for (auto &[usr, p] : u->vars_uses)
    updateUses(usr, Kind::Var, p, false, qs->entities_uses);

  for (auto &p : files) {
    qs->dbi_files.put(txn, lmdb::to_sv(p.first), fileToStr(p.second));
  }
#undef REMOVE_ADD
}

void DB::populateVFS(VFS *vfs) const {
  std::unordered_set<std::string> step1;
  std::lock_guard lock(vfs->mutex);
  foreachFile([&](const QueryFile &file) {
    if (!file.def)
      return true;
    vfs->state[file.def->path].loaded = 1;
    vfs->state[file.def->path].step = 0;
    vfs->state[file.def->path].timestamp = file.def->mtime;
    for (auto &dep : file.def->dependencies)
      step1.insert(dep);
    return true;
  });
  for (auto &dep : step1)
    vfs->state[dep].step = 1;
}

QueryFile DB::getFile(int file_id) const {
  if (std::string_view val; qs->dbi_files.get(txn, lmdb::to_sv(file_id), val))
    return strToFile(val);
  return {};
}

void DB::putFile(int file_id, QueryFile &file) {
  qs->dbi_files.put(txn, lmdb::to_sv(file_id), fileToStr(file));
}

ObjId DB::allocObjId() {
  ObjId id = [&]() {
    if (qs->dbi_entities_objects_freelist.size(txn) != 0) {
      lmdb::cursor cursor =
          lmdb::cursor::open(txn, qs->dbi_entities_objects_freelist);
      std::string_view sv;
      if (!cursor.get(sv, MDB_FIRST))
        throw std::runtime_error("Inconsistent entities objects db");
      auto nid = lmdb::from_sv<ObjId>(sv);
      cursor.del();
      return nid;
    } else {
      return ObjId{qs->dbi_entities_objects.size(txn)};
    }
  }();
  qs->dbi_entities_objects.put(txn, lmdb::to_sv(id), {});
  return id;
}

void DB::removeObj(ObjId obj_id) {
  qs->dbi_entities_objects.del(txn, lmdb::to_sv(obj_id));
  qs->dbi_entities_objects_freelist.put(txn, lmdb::to_sv(obj_id), {});
}

template <typename ET, typename Def>
void DB::insertEntityDef(ET &&e, Def &&def) {
  decltype(allocate(def, {})) existing;
  auto cursor = e.defCursor(*this);
  CCLS_CURSOR_FOREACH(cursor, def1) {
    if (def1.file_id == def.file_id) {
      existing = def1;
      cursor.del();
      break;
    }
  }
  auto pdef = allocate(def, existing);
  qs->entities_defs.put(txn, lmdb::to_sv(e.id), lmdb::to_sv(pdef));
}

template <typename Def>
void DB::removeEntityDef(SubdbCursor<EntityID, Def> &it) {
  const auto &def = *it;
  removeObj(def.detailed_name.obj_id);
  removeObj(def.hover.obj_id);
  removeObj(def.comments.obj_id);
  removeObj(def.bases.obj_id);
  if constexpr (DefToKind<Def>() == Kind::Func) {
    removeObj(def.vars.obj_id);
    removeObj(def.callees.obj_id);
  } else if constexpr (DefToKind<Def>() == Kind::Type) {
    removeObj(def.funcs.obj_id);
    removeObj(def.types.obj_id);
    removeObj(def.vars.obj_id);
  }
  it.del();
}

void DB::foreachFile(std::function<bool(const QueryFile &)> &fn) const {
  std::string_view key, val;
  auto cur = lmdb::cursor::open(txn, qs->dbi_files);
  while (cur.get(key, val, MDB_NEXT)) {
    QueryFile file = strToFile(val);
    if (!fn(file))
      break;
  }
}

size_t DB::dbiSubCount(lmdb::txn &txn, lmdb::dbi &dbi,
                       std::string_view key) const {
  lmdb::cursor cur = lmdb::cursor::open(txn, dbi);
  if (!cur.get(key, MDB_SET_KEY))
    return 0;
  return cur.count();
}

template <typename Key, typename Value>
std::remove_reference_t<Value> *DB::insertMap(lmdb::dbi &dbi, Key &&key,
                                              Value &&val, bool no_overwrite) {
  MDB_val key_mv{sizeof(std::remove_reference_t<Key>), &key},
      val_mv{sizeof(std::remove_reference_t<Value>), &val};
  if (!lmdb::dbi_put(txn, dbi, &key_mv, &val_mv,
                     no_overwrite ? MDB_NOOVERWRITE : 0))
    return static_cast<std::remove_reference_t<Value> *>(val_mv.mv_data);
  return nullptr;
}

template <typename Key, typename Value>
bool DB::removeMap(lmdb::dbi &dbi, Key &&key, Value &&val) {
  return dbi.del(txn, lmdb::to_sv(key), lmdb::to_sv(val));
}

int DB::getFileId(const std::string &path) {
  uint64_t hashed_name = hashUsr(lowerPathIfInsensitive(path));
  int id = qs->dbi_files.size(txn);
  if (auto cid = insertMap(qs->dbi_hashed_name2file_id, hashed_name, id, true);
      cid == nullptr) {
    qs->dbi_files.put(txn, lmdb::to_sv(id), fileToStr(QueryFile{id}));
  } else {
    id = *cid;
  }
  return id;
}

template <Kind kind>
std::pair<QueryEntityType<kind>, bool> DB::idPutEntity(Usr usr) {
  EntityID id = makeEntityID(kind, usr);
  QueryEntityType<kind> entity;
  entity.id = id;
  entity.usr = usr;
  entity.kind = kind;
  bool Result = qs->entities.put(txn, lmdb::to_sv(id), lmdb::to_sv(entity),
                                 MDB_NOOVERWRITE);
  if (Result)
    qs->kind_to_entity_id.put(txn, lmdb::to_sv(kind), lmdb::to_sv(id));
  return {entity, Result};
}

std::pair<QueryEntity, bool> DB::idPutEntity(Kind kind, Usr usr) {
  switch (kind) {
  case Kind::Func:
    return idPutEntity<Kind::Func>(usr);
  case Kind::Type:
    return idPutEntity<Kind::Type>(usr);
  case Kind::Var:
    return idPutEntity<Kind::Var>(usr);
  default:
    throw std::runtime_error("Inserting invalid kind of entities");
  }
}

void DB::idRemoveEntity(SubdbCursor<EntityID, QueryEntity> &it) {
  const QueryEntity &entity = *it;
  qs->entities_defs.del(txn, lmdb::to_sv(entity.id));
  qs->entities_declarations.del(txn, lmdb::to_sv(entity.id));
  qs->entities_uses.del(txn, lmdb::to_sv(entity.id));
  if (entity.kind == Kind::Func) {
    qs->entities_derived.del(txn, lmdb::to_sv(entity.id));
  } else if (entity.kind == Kind::Type) {
    qs->entities_derived.del(txn, lmdb::to_sv(entity.id));
    qs->entities_instances.del(txn, lmdb::to_sv(entity.id));
  }
  qs->kind_to_entity_id.del(txn, lmdb::to_sv(entity.kind),
                            lmdb::to_sv(entity.id));
  it.del();
}

std::optional<int> DB::findFileId(const std::string &path) const {
  uint64_t hashed_name = hashUsr(lowerPathIfInsensitive(path));
  if (std::string_view val;
      qs->dbi_hashed_name2file_id.get(txn, lmdb::to_sv(hashed_name), val)) {
    return lmdb::from_sv<int>(val);
  }
  return {};
}

int DB::update(std::unordered_map<int, QueryFile> &files,
               QueryFile::DefUpdate &&u) {
  int file_id = getFileId(u.first.path);
  QueryFile file;
  if (auto it = files.find(file_id); it == files.end())
    file = getFile(file_id);
  else
    file = it->second;
  file.def.emplace(u.first);
  files[file_id] = file;
  return file_id;
}

void DB::update(std::unordered_map<int, QueryFile> &files,
                const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, IUFuncDef>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id].symbol2refcnt[{
          {def.spell->range, u.first, Kind::Func, def.spell->role},
          def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity<Kind::Func>(u.first);
    insertEntityDef(entity, def);
  }
}

void DB::update(std::unordered_map<int, QueryFile> &files,
                const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, IUTypeDef>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id].symbol2refcnt[{
          {def.spell->range, u.first, Kind::Type, def.spell->role},
          def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity<Kind::Type>(u.first);
    insertEntityDef(entity, def);
  }
}

void DB::update(std::unordered_map<int, QueryFile> &files,
                const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, IUVarDef>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id].symbol2refcnt[{
          {def.spell->range, u.first, Kind::Var, def.spell->role},
          def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity<Kind::Var>(u.first);
    insertEntityDef(entity, def);
  }
}

std::string DB::getSymbolName(SymbolIdx sym, bool qualified) const {
  Usr usr = sym.usr;
  switch (sym.kind) {
  default:
    break;
  case Kind::File: {
    std::string_view val;
    if (qs->dbi_files.get(txn, lmdb::to_sv(usr), val)) {
      if (QueryFile file = strToFile(val); file.def)
        return file.def->path;
    }
  } break;
  case Kind::Func:
    if (auto def = getFunc(usr).getAnyDef(*this))
      return std::string(def->name(this, qualified));
    break;
  case Kind::Type:
    if (auto def = getType(usr).getAnyDef(*this))
      return std::string(def->name(this, qualified));
    break;
  case Kind::Var:
    if (auto def = getVar(usr).getAnyDef(*this))
      return std::string(def->name(this, qualified));
    break;
  }
  return "";
}

std::vector<uint8_t>
DB::getFileSet(const std::vector<std::string> &folders) const {
  if (folders.empty())
    return std::vector<uint8_t>(qs->dbi_files.size(txn), 1);
  std::vector<uint8_t> file_set(qs->dbi_files.size(txn));
  std::string_view key, val;
  auto cur = lmdb::cursor::open(txn, qs->dbi_files);
  while (cur.get(key, val, MDB_NEXT)) {
    QueryFile file = strToFile(val);
    if (file.def) {
      bool ok = false;
      for (auto &folder : folders)
        if (llvm::StringRef(file.def->path).startswith(folder)) {
          ok = true;
          break;
        }
      if (ok)
        file_set[file.id] = 1;
    }
  }
  return file_set;
}

bool DB::hasEntityId(Kind kind, Usr usr) const {
  EntityID id = makeEntityID(kind, usr);
  switch (kind) {
  case Kind::Func:
  case Kind::Type:
  case Kind::Var:
    break;
  case Kind::File:
  case Kind::Invalid:
    return false;
  }
  if (std::string_view val; !qs->entities.get(txn, lmdb::to_sv(id), val))
    return false;
  return true;
}

const QueryEntity &DB::id2Entity(EntityID id) const {
  std::string_view val;
  if (!qs->entities.get(txn, lmdb::to_sv(id), val))
    throw std::out_of_range(
        "Unexpected: Entity not found in entities database");
  return *lmdb::ptr_from_sv<QueryEntity>(val);
}

SubdbCursor<EntityID, QueryEntityDef>
DB::entityDefCursor(const QueryEntity &entity) const {
  return SubdbCursor<EntityID, QueryEntityDef>::makeCursor(
      txn, qs->entities_defs, entity.id);
}

SubdbCursor<EntityID, DeclRef>
DB::entityDeclCursor(const QueryEntity &entity) const {
  return SubdbCursor<EntityID, DeclRef>::makeCursor(
      txn, qs->entities_declarations, entity.id);
}

SubdbCursor<EntityID, Usr>
DB::entityDerivedCursor(const QueryEntity &entity) const {
  return SubdbCursor<EntityID, Usr>::makeCursor(txn, qs->entities_derived,
                                                entity.id);
}

SubdbCursor<EntityID, Usr>
DB::entityInstanceCursor(const QueryEntity &entity) const {
  return SubdbCursor<EntityID, Usr>::makeCursor(txn, qs->entities_instances,
                                                entity.id);
}

SubdbCursor<EntityID, Use>
DB::entityUseCursor(const QueryEntity &entity) const {
  return SubdbCursor<EntityID, Use>::makeCursor(txn, qs->entities_uses,
                                                entity.id);
}

namespace {
// Computes roughly how long |range| is.
int computeRangeSize(const Range &range) {
  if (range.start.line != range.end.line)
    return INT_MAX;
  return range.end.column - range.start.column;
}
} // namespace

Maybe<DeclRef> getDefinitionSpell(DB *db, SymbolIdx sym) {
  Maybe<DeclRef> ret;
  eachEntityDef(db, sym, [&](const auto &def) { return !(ret = def.spell); });
  return ret;
}

template <typename C>
std::vector<Use> getDeclarations(DB *db, Kind kind, C &&usrs) {
  std::vector<Use> ret;
  allOf(usrs, [&](Usr usr) {
    withEntity(db, {usr, kind}, [db, &ret](const auto &entity) {
      bool has_def = false;
      CCLS_CURSOR_FOREACH(entity.defCursor(*db), def) {
        if (def.spell) {
          ret.push_back(*def.spell);
          has_def = true;
        }
      }
      if (!has_def) {
        CCLS_CURSOR_FOREACH(entity.declCursor(*db), use) { ret.push_back(use); }
      }
    });
    return true;
  });
  return ret;
}

template <typename RangeType>
std::vector<DeclRef> getVarDeclarationsImpl(DB *db, RangeType &&usrs,
                                            unsigned kind) {
  std::vector<DeclRef> ret;
  allOf(usrs, [&](Usr usr) {
    bool has_def = false;
    auto entity = db->getFunc(usr);
    CCLS_CURSOR_FOREACH(entity.defCursor(*db), def) {
      if (def.spell) {
        has_def = true;
        // See messages/ccls_vars.cc
        if (def.kind == SymbolKind::Field) {
          if (!(kind & 1))
            break;
        } else if (def.kind == SymbolKind::Variable) {
          if (!(kind & 2))
            break;
        } else if (def.kind == SymbolKind::Parameter) {
          if (!(kind & 4))
            break;
        }
        ret.push_back(*def.spell);
        break;
      }
    }
    if (!has_def) {
      CCLS_CURSOR_FOREACH(entity.declCursor(*db), use) { ret.push_back(use); }
    }
    return true;
  });
  return ret;
}

std::vector<Use> getFuncDeclarations(DB *db, llvm::ArrayRef<Usr> usrs) {
  return getDeclarations(db, Kind::Func, usrs);
}
std::vector<Use> getTypeDeclarations(DB *db, llvm::ArrayRef<Usr> usrs) {
  return getDeclarations(db, Kind::Type, usrs);
}
std::vector<DeclRef> getVarDeclarations(DB *db, llvm::ArrayRef<Usr> usrs,
                                        unsigned kind) {
  return getVarDeclarationsImpl(db, usrs, kind);
}

std::vector<Use> getFuncDeclarations(DB *db, SubdbCursor<EntityID, Usr> usrs) {
  return getDeclarations(db, Kind::Func, usrs);
}
std::vector<Use> getTypeDeclarations(DB *db, SubdbCursor<EntityID, Usr> usrs) {
  return getDeclarations(db, Kind::Type, usrs);
}
std::vector<DeclRef> getVarDeclarations(DB *db, SubdbCursor<EntityID, Usr> usrs,
                                        unsigned kind) {
  return getVarDeclarationsImpl(db, usrs, kind);
}

std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym) {
  std::vector<DeclRef> result;
  withEntity(db, sym, [&](const auto &e) {
    CCLS_CURSOR_FOREACH(e.declCursor(*db), dr) { result.push_back(dr); }
  });
  return result;
}

std::vector<Use> getUsesForAllBases(DB *db, const QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<QueryFunc> stack{root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    QueryFunc func = stack.back();
    stack.pop_back();
    if (auto def = db->entityGetAnyDef(func)) {
      auto bases = def->bases.get(db);
      eachDefinedFunc(db, bases, [&](const QueryFunc &func1) {
        if (!seen.count(func1.usr)) {
          seen.insert(func1.usr);
          stack.push_back(func1);
          CCLS_CURSOR_FOREACH(func1.useCursor(*db), use) { ret.push_back(use); }
        }
      });
    }
  }

  return ret;
}

std::vector<Use> getUsesForAllDerived(DB *db, const QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<QueryFunc> stack{root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    QueryFunc func = stack.back();
    stack.pop_back();
    CCLS_CURSOR_FOREACH(func.derivedCursor(*db), usr) {
      if (!seen.count(usr)) {
        const QueryFunc &func1 = db->getFunc(usr);
        seen.insert(func1.usr);
        stack.push_back(func1);
        CCLS_CURSOR_FOREACH(func1.useCursor(*db), use) { ret.push_back(use); }
      }
    }
  }

  return ret;
}

std::optional<lsRange> getLsRange(WorkingFile *wfile, const Range &location) {
  if (!wfile || wfile->index_lines.empty())
    return lsRange{Position{location.start.line, location.start.column},
                   Position{location.end.line, location.end.column}};

  int start_column = location.start.column, end_column = location.end.column;
  std::optional<int> start = wfile->getBufferPosFromIndexPos(
      location.start.line, &start_column, false);
  std::optional<int> end =
      wfile->getBufferPosFromIndexPos(location.end.line, &end_column, true);
  if (!start || !end)
    return std::nullopt;

  // If remapping end fails (end can never be < start), just guess that the
  // final location didn't move. This only screws up the highlighted code
  // region if we guess wrong, so not a big deal.
  //
  // Remapping fails often in C++ since there are a lot of "};" at the end of
  // class/struct definitions.
  if (*end < *start)
    *end = *start + (location.end.line - location.start.line);
  if (*start == *end && start_column > end_column)
    end_column = start_column;

  return lsRange{Position{*start, start_column}, Position{*end, end_column}};
}

DocumentUri getLsDocumentUri(DB *db, int file_id, std::string *path) {
  QueryFile file = db->getFile(file_id);
  if (file.def) {
    *path = file.def->path;
    return DocumentUri::fromPath(*path);
  } else {
    *path = "";
    return DocumentUri::fromPath("");
  }
}

DocumentUri getLsDocumentUri(DB *db, int file_id) {
  QueryFile file = db->getFile(file_id);
  if (file.def) {
    return DocumentUri::fromPath(file.def->path);
  } else {
    return DocumentUri::fromPath("");
  }
}

std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles, Use use) {
  std::string path;
  DocumentUri uri = getLsDocumentUri(db, use.file_id, &path);
  std::optional<lsRange> range = getLsRange(wfiles->getFile(path), use.range);
  if (!range)
    return std::nullopt;
  return Location{uri, *range};
}

std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles,
                                      SymbolRef sym, int file_id) {
  return getLsLocation(db, wfiles, Use{{sym.range, sym.role}, file_id});
}

LocationLink getLocationLink(DB *db, WorkingFiles *wfiles, DeclRef dr) {
  std::string path;
  DocumentUri uri = getLsDocumentUri(db, dr.file_id, &path);
  if (auto range = getLsRange(wfiles->getFile(path), dr.range))
    if (auto extent = getLsRange(wfiles->getFile(path), dr.extent)) {
      LocationLink ret;
      ret.targetUri = uri.raw_uri;
      ret.targetSelectionRange = *range;
      ret.targetRange = extent->includes(*range) ? *extent : *range;
      return ret;
    }
  return {};
}

SymbolKind getSymbolKind(DB *db, SymbolIdx sym) {
  SymbolKind ret;
  if (sym.kind == Kind::File)
    ret = SymbolKind::File;
  else {
    ret = SymbolKind::Unknown;
    withEntity(db, sym, [&](const auto &entity) {
      CCLS_CURSOR_FOREACH(entity.defCursor(*db), def) {
        ret = def.kind;
        break;
      }
    });
  }
  return ret;
}

std::optional<SymbolInformation> getSymbolInfo(DB *db, SymbolIdx sym,
                                               bool detailed) {
  switch (sym.kind) {
  case Kind::Invalid:
    break;
  case Kind::File: {
    QueryFile file = db->getFile(sym.usr);
    if (!file.def)
      break;

    SymbolInformation info;
    info.name = file.def->path;
    info.kind = SymbolKind::File;
    return info;
  }
  default: {
    SymbolInformation info;
    eachEntityDef(db, sym, [&](const auto &def) {
      if (detailed)
        info.name = def.detailed_name.get(db);
      else
        info.name = def.name(db, true);
      info.kind = def.kind;
      return false;
    });
    return info;
  }
  }

  return std::nullopt;
}

std::vector<SymbolRef> findSymbolsAtLocation(WorkingFile *wfile,
                                             const QueryFile *file,
                                             Position &ls_pos, bool smallest) {
  std::vector<SymbolRef> symbols;
  // If multiVersion > 0, index may not exist and thus index_lines is empty.
  if (wfile && wfile->index_lines.size()) {
    if (auto line = wfile->getIndexPosFromBufferPos(ls_pos.line,
                                                    &ls_pos.character, false)) {
      ls_pos.line = *line;
    } else {
      ls_pos.line = -1;
      return {};
    }
  }

  for (auto [sym, refcnt] : file->symbol2refcnt)
    if (refcnt > 0 && sym.range.contains(ls_pos.line, ls_pos.character))
      symbols.push_back(sym);

  // Order shorter ranges first, since they are more detailed/precise. This is
  // important for macros which generate code so that we can resolving the
  // macro argument takes priority over the entire macro body.
  //
  // Order Kind::Var before Kind::Type. Macro calls are treated as Var
  // currently. If a macro expands to tokens led by a Kind::Type, the macro and
  // the Type have the same range. We want to find the macro definition instead
  // of the Type definition.
  //
  // Then order functions before other types, which makes goto definition work
  // better on constructors.
  std::sort(
      symbols.begin(), symbols.end(),
      [](const SymbolRef &a, const SymbolRef &b) {
        int t = computeRangeSize(a.range) - computeRangeSize(b.range);
        if (t)
          return t < 0;
        // MacroExpansion
        if ((t = (a.role & Role::Dynamic) - (b.role & Role::Dynamic)))
          return t > 0;
        if ((t = (a.role & Role::Definition) - (b.role & Role::Definition)))
          return t > 0;
        // operator> orders Var/Func before Type.
        t = static_cast<int>(a.kind) - static_cast<int>(b.kind);
        if (t)
          return t > 0;
        return a.usr < b.usr;
      });
  if (symbols.size() && smallest) {
    SymbolRef sym = symbols[0];
    for (size_t i = 1; i < symbols.size(); i++)
      if (!(sym.range == symbols[i].range && sym.kind == symbols[i].kind)) {
        symbols.resize(i);
        break;
      }
  }

  return symbols;
}
} // namespace ccls
