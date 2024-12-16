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
namespace {

void assignFileId(const Lid2file_id &lid2file_id, int file_id, Use &use) {
  if (use.file_id == -1)
    use.file_id = file_id;
  else
    use.file_id = lid2file_id.find(use.file_id)->second;
}

FileDef::IndexInclude convert(const IndexInclude &o) { return {o.line, o.resolved_path}; }

QueryFile::DefUpdate buildFileDefUpdate(IndexFile &&indexed) {
  FileDef def;
  def.path = std::move(indexed.path);
  for (auto s : indexed.args)
    def.args.emplace_back(s);
  for (auto &m : indexed.includes)
    def.includes.emplace_back(convert(m));
  def.skipped_ranges.set(indexed.skipped_ranges.begin(), indexed.skipped_ranges.end());
  def.dependencies.reserve(indexed.dependencies.size());
  for (auto &dep : indexed.dependencies)
    def.dependencies.push_back(dep.first.val().data()); // llvm 8 -> data()
  def.language = indexed.language;
  def.mtime = indexed.mtime;
  return {std::move(def), std::move(indexed.file_contents)};
}

} // namespace

EntityID::EntityID(Kind kind, Usr usr) : hash(cista::hash_combine(cista::BASE_HASH, kind, usr)) {}

template <typename T> cista_types::vector<T> convert(const std::vector<T> &o) { return {o.begin(), o.end()}; }

QueryFunc::Def convert(const IndexFunc::Def &o) {
  QueryFunc::Def r;
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

QueryType::Def convert(const IndexType::Def &o) {
  QueryType::Def r;
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

QueryVar::Def convert(const IndexVar::Def &o) {
  QueryVar::Def r;
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

QueryStore::QueryStore(std::string_view store_path)
    : env(std::move(
          env.create().set_max_dbs(48).open(store_path.data(), MDB_WRITEMAP | MDB_NOMETASYNC | MDB_WRITEMAP_FSYNC))) {
  lmdb::txn txn(lmdb::txn::begin(env, nullptr, 0));
  dbi_files = lmdb::dbi::open(txn, "files", MDB_INTEGERKEY | MDB_CREATE);
  dbi_name2file_id = lmdb::dbi::open(txn, "name2file_id", MDB_CREATE);

  entities = lmdb::dbi::open(txn, "dbi_entities", MDB_CREATE);
  entities_defs = lmdb::dbi::open(txn, "dbi_entities_def", MDB_DUPSORT | MDB_CREATE);
  entities_declarations = lmdb::dbi::open(txn, "dbi_entities_declarations", MDB_DUPSORT | MDB_CREATE);
  entities_derived = lmdb::dbi::open(txn, "dbi_entities_derived", MDB_DUPSORT | MDB_CREATE);
  entities_instances = lmdb::dbi::open(txn, "dbi_entities_instances", MDB_DUPSORT | MDB_CREATE);
  entities_uses = lmdb::dbi::open(txn, "dbi_entities_uses", MDB_DUPSORT | MDB_CREATE);
  kind_to_entity_id = lmdb::dbi::open(txn, "kind_to_entity_id", MDB_DUPSORT | MDB_CREATE);

  txn.commit();
}

void QueryStore::increaseMapSize() {
  MDB_envinfo envinfo;
  lmdb::env_info(env, &envinfo);
  mdb_size_t increment = std::max(envinfo.me_mapsize, mdb_size_t(128) * 1024 * 1024);
  env.set_mapsize(envinfo.me_mapsize + increment);
}

void DB::Updater::clear() {
  qs_->dbi_files.drop(txn_);
  qs_->dbi_name2file_id.drop(txn_);
  qs_->entities.drop(txn_);
  qs_->entities_defs.drop(txn_);
  qs_->entities_declarations.drop(txn_);
  qs_->entities_derived.drop(txn_);
  qs_->entities_instances.drop(txn_);
  qs_->entities_uses.drop(txn_);
  qs_->kind_to_entity_id.drop(txn_);
}

int DB::Updater::getFileId(std::string_view path) {
  std::string normalized_name = lowerPathIfInsensitive(path);
  int id = qs_->dbi_files.size(txn_);
  MDB_val key{.mv_size = normalized_name.size(), .mv_data = normalized_name.data()},
      val{.mv_size = sizeof(id), .mv_data = &id};
  if (!lmdb::dbi_put(txn_, qs_->dbi_name2file_id, &key, &val, MDB_NOOVERWRITE)) {
    id = *static_cast<decltype(id) *>(val.mv_data);
  } else {
    QueryFile file{id};
    putFile(id, file);
  }
  return id;
}

void DB::Updater::putFile(int file_id, QueryFile &file) {
  auto buf = cista::serialize(file);
  qs_->dbi_files.put(txn_, lmdb::to_sv(file_id),
                     std::string_view(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

int DB::Updater::update(std::unordered_map<int, QueryFile> &files, QueryFile::DefUpdate &&u) {
  int file_id = getFileId(u.first.path);
  QueryFile file;
  if (auto it = files.find(file_id); it == files.end())
    file = db_.getFile(file_id);
  else
    file = it->second;
  file.def = u.first;
  files[file_id] = file;
  return file_id;
}

template <typename ET, typename Def> void DB::Updater::insertEntityDef(ET &&e, Def &&def) {
  lmdb::cursor cur = lmdb::cursor::open(txn_, qs_->entities_defs);
  std::string_view key = lmdb::to_sv(e.id), val;
  for (auto op = MDB_SET_KEY; cur.get(key, val, op); op = MDB_NEXT_DUP) {
    typedef std::remove_reference_t<Def> _Def;
    const _Def *p = reinterpret_cast<const _Def *>(val.data());
    if (p->file_id == def.file_id)
      cur.del();
  }
  auto buf = cista::serialize(def);
  cur.put(lmdb::to_sv(e.id), std::string_view(reinterpret_cast<const char *>(buf.data()), buf.size()));
}

std::pair<QueryEntity, bool> DB::Updater::idPutEntity(Kind kind, Usr usr) {
  if (kind != Kind::Func && kind != Kind::Type && kind != Kind::Var)
    throw std::runtime_error("Inserting invalid kind of entities");
  EntityID id(kind, usr);
  QueryEntity entity{id, usr, kind};
  entity.id = id;
  entity.usr = usr;
  entity.kind = kind;
  bool Result = qs_->entities.put(txn_, lmdb::to_sv(id), lmdb::to_sv(entity), MDB_NOOVERWRITE);
  if (Result)
    qs_->kind_to_entity_id.put(txn_, lmdb::to_sv(kind), lmdb::to_sv(id));
  return {entity, Result};
}

void DB::Updater::idRemoveEntity(SubdbIterator<EntityID, QueryEntity> &&it) {
  const QueryEntity &entity = *it;
  qs_->entities_defs.del(txn_, lmdb::to_sv(entity.id));
  qs_->entities_declarations.del(txn_, lmdb::to_sv(entity.id));
  qs_->entities_uses.del(txn_, lmdb::to_sv(entity.id));
  if (entity.kind == Kind::Func) {
    qs_->entities_derived.del(txn_, lmdb::to_sv(entity.id));
  } else if (entity.kind == Kind::Type) {
    qs_->entities_derived.del(txn_, lmdb::to_sv(entity.id));
    qs_->entities_instances.del(txn_, lmdb::to_sv(entity.id));
  }
  qs_->kind_to_entity_id.del(txn_, lmdb::to_sv(entity.kind), lmdb::to_sv(entity.id));
  qs_->entities.del(txn_, lmdb::to_sv(entity.id));
}

void DB::Updater::update(std::unordered_map<int, QueryFile> &files, const Lid2file_id &lid2file_id, int file_id,
                         std::vector<std::pair<Usr, QueryFunc::Def>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id]
          .symbol2refcnt[{{def.spell->range, u.first, Kind::Func, def.spell->role}, def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity(Kind::Func, u.first);
    insertEntityDef(entity, def);
  }
}

void DB::Updater::update(std::unordered_map<int, QueryFile> &files, const Lid2file_id &lid2file_id, int file_id,
                         std::vector<std::pair<Usr, QueryType::Def>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id]
          .symbol2refcnt[{{def.spell->range, u.first, Kind::Type, def.spell->role}, def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity(Kind::Type, u.first);
    insertEntityDef(entity, def);
  }
}

void DB::Updater::update(std::unordered_map<int, QueryFile> &files, const Lid2file_id &lid2file_id, int file_id,
                         std::vector<std::pair<Usr, QueryVar::Def>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(def.detailed_name[0]);
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id]
          .symbol2refcnt[{{def.spell->range, u.first, Kind::Var, def.spell->role}, def.spell->extent}]++;
    }
    auto [entity, _] = idPutEntity(Kind::Var, u.first);
    insertEntityDef(entity, def);
  }
}

template <typename Def> void DB::Updater::removeUsrs(int file_id, const std::vector<std::pair<Usr, Def>> &to_remove) {
  constexpr Kind kind =
      std::is_same_v<Def, QueryFunc::Def> ? Kind::Func : (std::is_same_v<Def, QueryType::Def> ? Kind::Type : Kind::Var);
  for (auto &[usr, _] : to_remove) {
    if (!db_.hasEntityId(kind, usr))
      continue;
    EntityID id(kind, usr);
    QueryEntityDef tdef;
    tdef.file_id = file_id;
    qs_->entities_defs.del(txn_, lmdb::to_sv(id), lmdb::to_sv(tdef));
  }
}

void DB::Updater::applyIndexUpdate(IndexUpdate *u) {
#define REMOVE_ADD(Kind, C, F)                                                                                         \
  for (auto &it : u->C##s_##F) {                                                                                       \
    auto usr = it.first;                                                                                               \
    auto e = idPutEntity(Kind, usr).first;                                                                             \
    for (auto &v : it.second.first)                                                                                    \
      qs_->entities_##F.del(txn_, lmdb::to_sv(e.id), lmdb::to_sv(v));                                                  \
    for (auto &v : it.second.second)                                                                                   \
      qs_->entities_##F.put(txn_, lmdb::to_sv(e.id), lmdb::to_sv(v));                                                  \
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
      file = db_.getFile(file_id);
    lid2file_id[lid] = file_id;
    if (!file.def) {
      file.def = {};
      file.def->path = path;
    }
  }

  // References (Use &use) in this function are important to update file_id.
  auto ref = [&](std::unordered_map<int, int> &lid2fid, Usr usr, Kind kind, Use &use, int delta) {
    use.file_id = use.file_id == -1 ? u->file_id : lid2fid.find(use.file_id)->second;
    ExtentRef sym{{use.range, usr, kind, use.role}};
    int &v = files[use.file_id].symbol2refcnt[sym];
    v += delta;
    assert(v >= 0);
    if (!v)
      files[use.file_id].symbol2refcnt.erase(sym);
  };
  auto refDecl = [&](std::unordered_map<int, int> &lid2fid, Usr usr, Kind kind, DeclRef &dr, int delta) {
    dr.file_id = dr.file_id == -1 ? u->file_id : lid2fid.find(dr.file_id)->second;
    ExtentRef sym{{dr.range, usr, kind, dr.role}, dr.extent};
    int &v = files[dr.file_id].symbol2refcnt[sym];
    v += delta;
    assert(v >= 0);
    if (!v)
      files[dr.file_id].symbol2refcnt.erase(sym);
  };

  auto updateUses = [&](Usr usr, Kind kind, auto &p, bool hint_implicit, lmdb::dbi &dbi_entities_uses) {
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
      dbi_entities_uses.del(txn_, lmdb::to_sv(entity.id), lmdb::to_sv(use));
    for (Use &use : p.second) {
      if (hint_implicit && use.role & Role::Implicit) {
        if (use.range.start.column > 0)
          use.range.start.column--;
        use.range.end.column++;
      }
      ref(lid2file_id, usr, kind, use, 1);
    }
    for (Use &use : p.second)
      dbi_entities_uses.put(txn_, lmdb::to_sv(entity.id), lmdb::to_sv(use));
  };

  if (u->files_removed) {
    std::string normalized_name = lowerPathIfInsensitive(*u->files_removed);
    if (std::string_view val; qs_->dbi_name2file_id.get(txn_, normalized_name, val)) {
      int file_id = lmdb::from_sv<int>(val);
      files[file_id].def = decltype(files[file_id].def)();
    }
  }
  u->file_id = u->files_def_update ? update(files, std::move(*u->files_def_update)) : -1;

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
    updateUses(usr, Kind::Func, p, true, qs_->entities_uses);

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
    updateUses(usr, Kind::Type, p, false, qs_->entities_uses);

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
    updateUses(usr, Kind::Var, p, false, qs_->entities_uses);

  for (auto &p : files)
    putFile(p.first, p.second);
#undef REMOVE_ADD
}

void DB::populateVFS(VFS *vfs) const {
  std::unordered_set<std::string> step1;
  std::lock_guard lock(vfs->mutex);
  for (const auto &file : files()) {
    if (!file.def)
      continue;
    vfs->state[file.def->path.str()].loaded = 1;
    vfs->state[file.def->path.str()].step = 0;
    vfs->state[file.def->path.str()].timestamp = file.def->mtime;
    for (auto &dep : file.def->dependencies)
      step1.insert(dep.str());
  }
  for (auto &dep : step1)
    vfs->state[dep].step = 1;
}

const QueryFile &DB::getFile(int file_id) const {
  static QueryFile empty;
  if (std::string_view val; qs_->dbi_files.get(txn_, lmdb::to_sv(file_id), val))
    return *lmdb::ptr_from_sv<QueryFile>(val);
  return empty;
}

std::optional<int> DB::findFileId(const std::string &path) const {
  std::string normalized_name = lowerPathIfInsensitive(path);
  if (std::string_view val; qs_->dbi_name2file_id.get(txn_, normalized_name, val)) {
    return lmdb::from_sv<int>(val);
  }
  return {};
}

std::string DB::getSymbolName(SymbolIdx sym, bool qualified) const {
  Usr usr = sym.usr;
  switch (sym.kind) {
  default:
    break;
  case Kind::File: {
    std::string_view val;
    if (qs_->dbi_files.get(txn_, lmdb::to_sv(usr), val)) {
      if (const auto &file = *lmdb::ptr_from_sv<QueryFile>(val); file.def)
        return file.def->path.str();
    }
  } break;
  case Kind::Func:
    if (const auto &def = getFunc(usr).anyDef(this))
      return std::string(def->name(qualified));
    break;
  case Kind::Type:
    if (const auto &def = getType(usr).anyDef(this))
      return std::string(def->name(qualified));
    break;
  case Kind::Var:
    if (const auto &def = getVar(usr).anyDef(this))
      return std::string(def->name(qualified));
    break;
  }
  return "";
}

std::vector<uint8_t> DB::getFileSet(const std::vector<std::string> &folders) const {
  if (folders.empty())
    return std::vector<uint8_t>(qs_->dbi_files.size(txn_), 1);
  std::vector<uint8_t> file_set(qs_->dbi_files.size(txn_));
  std::string_view key, val;
  auto cur = lmdb::cursor::open(txn_, qs_->dbi_files);
  while (cur.get(key, val, MDB_NEXT)) {
    const auto &file = *lmdb::ptr_from_sv<QueryFile>(val);
    if (file.def) {
      bool ok = false;
      for (auto &folder : folders)
        if (llvm::StringRef(file.def->path).starts_with(folder)) {
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
  EntityID id(kind, usr);
  switch (kind) {
  case Kind::Func:
  case Kind::Type:
  case Kind::Var:
    break;
  case Kind::File:
  case Kind::Invalid:
    return false;
  }
  if (std::string_view v; !qs_->entities.get(txn_, lmdb::to_sv(id), v))
    return false;
  return true;
}

const QueryEntity &DB::id2Entity(EntityID id) const {
  std::string_view val;
  if (!qs_->entities.get(txn_, lmdb::to_sv(id), val))
    throw std::out_of_range("Unexpected: Entity not found in entities database");
  return *lmdb::ptr_from_sv<QueryEntity>(val);
}

SubdbContainer<EntityID, QueryEntityDef> DB::entityDefs(const QueryEntity &entity) const {
  return SubdbContainer<EntityID, QueryEntityDef>(txn_, qs_->entities_defs, entity.id);
}
SubdbContainer<EntityID, DeclRef> DB::entityDecls(const QueryEntity &entity) const {
  return SubdbContainer<EntityID, DeclRef>(txn_, qs_->entities_declarations, entity.id);
}
SubdbContainer<EntityID, Usr> DB::entityDeriveds(const QueryEntity &entity) const {
  return SubdbContainer<EntityID, Usr>(txn_, qs_->entities_derived, entity.id);
}
SubdbContainer<EntityID, Usr> DB::entityInstances(const QueryEntity &entity) const {
  return SubdbContainer<EntityID, Usr>(txn_, qs_->entities_instances, entity.id);
}
SubdbContainer<EntityID, Use> DB::entityUses(const QueryEntity &entity) const {
  return SubdbContainer<EntityID, Use>(txn_, qs_->entities_uses, entity.id);
}

DbContainer<int, QueryFile> DB::files() const { return DbContainer<int, QueryFile>(txn_, qs_->dbi_files); }
SubdbContainer<Kind, EntityID> DB::allUsrs(Kind kind) const {
  return SubdbContainer<Kind, EntityID>(txn_, qs_->kind_to_entity_id, kind);
}

TxnManager TxnManager::begin(QueryStoreConnection qs, bool read_only) {
  TxnManager Result;
  Result.qs_ = qs;
  Result.txn_ = [&qs, read_only]() {
    auto txn = std::make_unique<lmdb::txn>(nullptr);
    do {
      try {
        *txn = lmdb::txn::begin(qs->env, nullptr, read_only ? MDB_RDONLY : 0);
      } catch (const lmdb::runtime_error &e) {
        if (e.code() != MDB_MAP_RESIZED)
          throw;
        qs->env.set_mapsize(0);
      }
    } while (txn->handle() == nullptr);
    return txn;
  }();
  Result.db_ = std::make_unique<DB>(qs, *Result.txn_);
  return Result;
}

void TxnManager::commit() && {
  txn_->commit();
  db_.reset();
}

void TxnManager::abort() && noexcept {
  txn_->abort();
  db_.reset();
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

template <typename C> std::vector<Use> getDeclarations(DB *db, Kind kind, C &&usrs) {
  std::vector<Use> ret;
  allOf(usrs, [&](Usr usr) {
    withEntity(db, {usr, kind}, [db, &ret](const auto &entity) {
      bool has_def = false;
      auto defs = entity.defs(db);
      for (const auto &def : entity.defs(db)) {
        if (def.spell) {
          ret.push_back(*def.spell);
          has_def = true;
        }
      }
      if (!has_def) {
        for (const auto &dr : entity.decls(db))
          ret.push_back(dr);
      }
    });
    return true;
  });
  return ret;
}

template <typename RangeType> std::vector<DeclRef> getVarDeclarationsImpl(DB *db, RangeType &&usrs, unsigned kind) {
  std::vector<DeclRef> ret;
  forEach(usrs, [&](Usr usr) {
    bool has_def = false;
    auto entity = db->getFunc(usr);
    for (const auto &def : entity.defs(db)) {
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
      for (const auto &dr : entity.decls(db))
        ret.push_back(dr);
    }
  });
  return ret;
}

std::vector<Use> getFuncDeclarations(DB *db, llvm::ArrayRef<Usr> usrs) { return getDeclarations(db, Kind::Func, usrs); }
std::vector<Use> getTypeDeclarations(DB *db, llvm::ArrayRef<Usr> usrs) { return getDeclarations(db, Kind::Type, usrs); }
std::vector<DeclRef> getVarDeclarations(DB *db, llvm::ArrayRef<Usr> usrs, unsigned kind) {
  return getVarDeclarationsImpl(db, usrs, kind);
}

std::vector<Use> getFuncDeclarations(DB *db, SubdbContainer<EntityID, Usr> &usrs) {
  return getDeclarations(db, Kind::Func, usrs);
}
std::vector<Use> getTypeDeclarations(DB *db, SubdbContainer<EntityID, Usr> &usrs) {
  return getDeclarations(db, Kind::Type, usrs);
}
std::vector<DeclRef> getVarDeclarations(DB *db, SubdbContainer<EntityID, Usr> &usrs, unsigned kind) {
  return getVarDeclarationsImpl(db, usrs, kind);
}

std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym) {
  std::vector<DeclRef> ret;
  withEntity(db, sym, [&](const auto &e) {
    for (const auto &dr : e.decls(db))
      ret.push_back(dr);
  });
  return ret;
}

std::vector<Use> getUsesForAllBases(DB *db, const QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<const QueryFunc *> stack{&root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    const QueryFunc &func = *stack.back();
    stack.pop_back();
    if (auto def = func.anyDef(db)) {
      const auto &bases = def->bases;
      eachDefinedFunc(db, bases, [&](const QueryFunc &func1) {
        if (!seen.count(func1.usr)) {
          seen.insert(func1.usr);
          stack.push_back(&func1);
          for (const auto &use : func1.uses(db))
            ret.push_back(use);
        }
      });
    }
  }

  return ret;
}

std::vector<Use> getUsesForAllDerived(DB *db, const QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<const QueryFunc *> stack{&root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    const QueryFunc &func = *stack.back();
    stack.pop_back();
    eachDefinedFunc(db, func.deriveds(db), [&](const QueryFunc &func1) {
      if (!seen.count(func1.usr)) {
        seen.insert(func1.usr);
        stack.push_back(&func1);
        for (const auto &use : func1.uses(db))
          ret.push_back(use);
      }
    });
  }

  return ret;
}

std::optional<lsRange> getLsRange(WorkingFile *wfile, const Range &location) {
  if (!wfile || wfile->index_lines.empty())
    return lsRange{Position{location.start.line, location.start.column},
                   Position{location.end.line, location.end.column}};

  int start_column = location.start.column, end_column = location.end.column;
  std::optional<int> start = wfile->getBufferPosFromIndexPos(location.start.line, &start_column, false);
  std::optional<int> end = wfile->getBufferPosFromIndexPos(location.end.line, &end_column, true);
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
  const QueryFile &file = db->getFile(file_id);
  if (file.def) {
    *path = file.def->path;
    return DocumentUri::fromPath(*path);
  } else {
    *path = "";
    return DocumentUri::fromPath("");
  }
}

DocumentUri getLsDocumentUri(DB *db, int file_id) {
  const QueryFile &file = db->getFile(file_id);
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

std::optional<Location> getLsLocation(DB *db, WorkingFiles *wfiles, SymbolRef sym, int file_id) {
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
      auto defs = entity.defs(db);
      if (defs.size() != 0)
        ret = defs.begin()->kind;
    });
  }
  return ret;
}

std::optional<SymbolInformation> getSymbolInfo(DB *db, SymbolIdx sym, bool detailed) {
  switch (sym.kind) {
  case Kind::Invalid:
    break;
  case Kind::File: {
    const QueryFile &file = db->getFile(sym.usr);
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
        info.name = def.detailed_name;
      else
        info.name = def.name(true);
      info.kind = def.kind;
      return false;
    });
    return info;
  }
  }

  return std::nullopt;
}

std::vector<SymbolRef> findSymbolsAtLocation(WorkingFile *wfile, const QueryFile *file, Position &ls_pos,
                                             bool smallest) {
  std::vector<SymbolRef> symbols;
  // If multiVersion > 0, index may not exist and thus index_lines is empty.
  if (wfile && wfile->index_lines.size()) {
    if (auto line = wfile->getIndexPosFromBufferPos(ls_pos.line, &ls_pos.character, false)) {
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
  std::sort(symbols.begin(), symbols.end(), [](const SymbolRef &a, const SymbolRef &b) {
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
