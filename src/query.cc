// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "query.hh"

#include "pipeline.hh"
#include "serializer.hh"

#include <llvm/ADT/ScopeExit.h>

#include <rapidjson/document.h>
#include <siphash.h>

#include <assert.h>
#include <functional>
#include <limits.h>
#include <optional>
#include <stack>
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

template <typename T>
void addRange(db::scoped_vector<T> &into, const std::vector<T> &from) {
  into.insert(into.end(), from.begin(), from.end());
}

template <typename T>
void removeRange(db::scoped_vector<T> &from, const std::vector<T> &to_remove) {
  if (to_remove.size()) {
    std::unordered_set<T> to_remove_set(to_remove.begin(), to_remove.end());
    from.erase(
        std::remove_if(from.begin(), from.end(),
                       [&](const T &t) { return to_remove_set.count(t) > 0; }),
        from.end());
  }
}

QueryFile::DefUpdate buildFileDefUpdate(IndexFile &&indexed) {
  auto moveArray = [](auto &&from, auto &to) {
    to.clear();
    to.reserve(from.size());
    std::for_each(from.begin(), from.end(),
                  [&to](auto &i) { to.push_back(i); });
  };
  QueryFile::CoreDef def;
  def.path = db::toInMemScopedString(indexed.path);
  moveArray(indexed.args, def.args);
  moveArray(indexed.includes, def.includes);
  moveArray(indexed.skipped_ranges, def.skipped_ranges);
  def.dependencies.reserve(indexed.dependencies.size());
  for (auto &dep : indexed.dependencies)
    def.dependencies.push_back(dep.first.val().data()); // llvm 8 -> data()
  def.language = indexed.language;
  return {std::move(def), std::move(indexed.file_contents)};
}

template <typename T, template <typename...> typename V>
db::scoped_vector<T> convert(const V<T> &o) {
  db::scoped_vector<T> r(db::getAlloc());
  r.reserve(o.size());
  for (auto it = o.begin(); it != o.end(); it++)
    r.push_back(*it);
  return r;
}

// Returns true if an element with the same file is found.
template <typename Q>
bool tryReplaceDef(db::scoped_vector<Q> &def_list, Q &&def) {
  for (auto &def1 : def_list)
    if (def1.file_id == def.file_id) {
      def1 = std::move(def);
      return true;
    }
  return false;
}

thread_local std::unordered_set<QueryStore *> t_writing_txn;
} // namespace

QueryFile::Def convert(const QueryFile::CoreDef &o) {
  QueryFile::Def r(db::getAlloc());
  r.path = db::toInMemScopedString(o.path);
  std::for_each(o.args.begin(), o.args.end(), [&r](const auto &m) {
    r.args.push_back(db::toInMemScopedString(m));
  });
  r.language = o.language;
  std::for_each(o.includes.begin(), o.includes.end(),
                [&r](const QueryFile::CoreDef::IndexInclude &m) {
                  QueryFile::Def::IndexInclude def(db::getAlloc());
                  def.line = m.line;
                  def.resolved_path = m.resolved_path;
                  r.includes.push_back(def);
                });
  r.skipped_ranges = convert(o.skipped_ranges);
  std::for_each(o.dependencies.begin(), o.dependencies.end(),
                [&r](const auto &m) {
                  r.dependencies.push_back(db::toInMemScopedString(m));
                });
  return r;
}

QueryFunc::Def convert(const IndexFunc::Def &o) {
  QueryFunc::Def r(db::getAlloc());
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
  QueryType::Def r(db::getAlloc());
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
  QueryVar::Def r(db::getAlloc());
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

QueryStore::QueryStore(const db::allocator<QueryStore> &alloc, bool priv)
    : identity(priv ? 0 : generateDBIdentity()), files(alloc),
      name2file_id(alloc), func_usr(alloc), type_usr(alloc), var_usr(alloc),
      funcs(alloc), types(alloc), vars(alloc), allocator(alloc) {}

void DB::clear() {
  files.clear();
  name2file_id.clear();
  func_usr.clear();
  type_usr.clear();
  var_usr.clear();
  funcs.clear();
  types.clear();
  vars.clear();
}

void QueryStore::startRead(std::function<void(DB *db)> &&fn) {
  DB *db = static_cast<DB *>(this);
  auto mtx = getSharableMutex(identity);
  bool locked = false;
  auto scope_exit = llvm::make_scope_exit([&]() {
    if (!locked)
      return;
    mtx->unlock_sharable();
  });
  if (mtx != nullptr || t_writing_txn.count(this) != 0) {
    fn(db);
  } else {
    mtx->lock_sharable();
    locked = true;
    try {
      fn(db);
    } catch (...) {
      throw;
    }
  }
}
void QueryStore::startWrite(std::function<void(DB *db)> &&fn) {
  DB *db = static_cast<DB *>(this);
  auto mtx = getSharableMutex(identity);
  bool locked = false;
  auto scope_exit = llvm::make_scope_exit([&]() {
    if (!locked)
      return;
    mtx->unlock();
    t_writing_txn.erase(this);
  });
  if (mtx == nullptr || !t_writing_txn.emplace(this).second) {
    fn(db);
  } else {
    mtx->lock();
    locked = true;
    try {
      fn(db);
    } catch (...) {
      throw;
    }
  }
}

template <typename Def>
void DB::removeUsrs(Kind kind, int file_id,
                    const std::vector<std::pair<Usr, Def>> &to_remove) {
  switch (kind) {
  case Kind::Func: {
    for (auto &[usr, _] : to_remove) {
      // FIXME
      if (!hasFunc(usr))
        continue;
      QueryFunc &func = getFunc(usr);
      auto it = llvm::find_if(func.def, [=](const QueryFunc::Def &def) {
        return def.file_id == file_id;
      });
      if (it != func.def.end())
        func.def.erase(it);
    }
    break;
  }
  case Kind::Type: {
    for (auto &[usr, _] : to_remove) {
      // FIXME
      if (!hasType(usr))
        continue;
      QueryType &type = getType(usr);
      auto it = llvm::find_if(type.def, [=](const QueryType::Def &def) {
        return def.file_id == file_id;
      });
      if (it != type.def.end())
        type.def.erase(it);
    }
    break;
  }
  case Kind::Var: {
    for (auto &[usr, _] : to_remove) {
      // FIXME
      if (!hasVar(usr))
        continue;
      QueryVar &var = getVar(usr);
      auto it = llvm::find_if(var.def, [=](const QueryVar::Def &def) {
        return def.file_id == file_id;
      });
      if (it != var.def.end())
        var.def.erase(it);
    }
    break;
  }
  default:
    break;
  }
}

void DB::applyIndexUpdate(IndexUpdate *u) {
#define REMOVE_ADD(C, F)                                                       \
  for (auto &it : u->C##s_##F) {                                               \
    auto r = C##_usr.try_emplace({it.first}, C##_usr.size());                  \
    if (r.second)                                                              \
      C##s.emplace_back().usr = it.first;                                      \
    auto &entity = C##s[r.first->second];                                      \
    removeRange(entity.F, it.second.first);                                    \
    addRange(entity.F, it.second.second);                                      \
  }

  std::unordered_map<int, int> prev_lid2file_id, lid2file_id;
  for (auto &[lid, path] : u->prev_lid2path)
    prev_lid2file_id[lid] = getFileId(path);
  for (auto &[lid, path] : u->lid2path) {
    int file_id = getFileId(path);
    lid2file_id[lid] = file_id;
    if (!files[file_id].def) {
      files[file_id].def.emplace(allocator);
      files[file_id].def->path = db::toInMemScopedString(path);
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

  auto updateUses =
      [&](Usr usr, Kind kind,
          ccls::db::scoped_unordered_map<Usr, std::size_t> &entity_usr,
          auto &entities, auto &p, bool hint_implicit) {
        auto r = entity_usr.try_emplace(usr, entity_usr.size());
        if (r.second)
          entities.emplace_back().usr = usr;
        auto &entity = entities[r.first->second];
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
        removeRange(entity.uses, p.first);
        for (Use &use : p.second) {
          if (hint_implicit && use.role & Role::Implicit) {
            if (use.range.start.column > 0)
              use.range.start.column--;
            use.range.end.column++;
          }
          ref(lid2file_id, usr, kind, use, 1);
        }
        addRange(entity.uses, p.second);
      };

  if (u->files_removed) {
    auto it = name2file_id.find(
        db::toInMemScopedString(lowerPathIfInsensitive(*u->files_removed)));
    if (it != name2file_id.end())
      files[it->second].def = std::nullopt;
  }
  u->file_id =
      u->files_def_update ? update(std::move(*u->files_def_update)) : -1;

  for (auto &[usr, def] : u->funcs_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Func, *def.spell, -1);
  removeUsrs(Kind::Func, u->file_id, u->funcs_removed);
  update(lid2file_id, u->file_id, std::move(u->funcs_def_update));
  for (auto &[usr, del_add] : u->funcs_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Func, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Func, dr, 1);
  }
  REMOVE_ADD(func, declarations);
  REMOVE_ADD(func, derived);
  for (auto &[usr, p] : u->funcs_uses)
    updateUses(usr, Kind::Func, func_usr, funcs, p, true);

  for (auto &[usr, def] : u->types_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Type, *def.spell, -1);
  removeUsrs(Kind::Type, u->file_id, u->types_removed);
  update(lid2file_id, u->file_id, std::move(u->types_def_update));
  for (auto &[usr, del_add] : u->types_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Type, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Type, dr, 1);
  }
  REMOVE_ADD(type, declarations);
  REMOVE_ADD(type, derived);
  REMOVE_ADD(type, instances);
  for (auto &[usr, p] : u->types_uses)
    updateUses(usr, Kind::Type, type_usr, types, p, false);

  for (auto &[usr, def] : u->vars_removed)
    if (def.spell)
      refDecl(prev_lid2file_id, usr, Kind::Var, *def.spell, -1);
  removeUsrs(Kind::Var, u->file_id, u->vars_removed);
  update(lid2file_id, u->file_id, std::move(u->vars_def_update));
  for (auto &[usr, del_add] : u->vars_declarations) {
    for (DeclRef &dr : del_add.first)
      refDecl(prev_lid2file_id, usr, Kind::Var, dr, -1);
    for (DeclRef &dr : del_add.second)
      refDecl(lid2file_id, usr, Kind::Var, dr, 1);
  }
  REMOVE_ADD(var, declarations);
  for (auto &[usr, p] : u->vars_uses)
    updateUses(usr, Kind::Var, var_usr, vars, p, false);

#undef REMOVE_ADD
}

int DB::getFileId(const std::string &path) {
  db::scoped_string norm_path(
      db::toInMemScopedString(lowerPathIfInsensitive(path)));
  size_t id = files.size();
  auto it = name2file_id.try_emplace(norm_path, id);
  if (it.second)
    files.emplace_back().id = id;
  return it.first->second;
}

int DB::update(QueryFile::DefUpdate &&u) {
  int file_id = getFileId(u.first.path);
  QueryFile &file = files[file_id];
  file.def.emplace(allocator) = convert(u.first);
  return file_id;
}

void DB::update(const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, QueryFunc::Def>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(!std::string_view(def.detailed_name).empty());
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id].symbol2refcnt[{
          {def.spell->range, u.first, Kind::Func, def.spell->role},
          def.spell->extent}]++;
    }

    auto r = func_usr.try_emplace({u.first}, func_usr.size());
    if (r.second)
      funcs.emplace_back();
    QueryFunc &existing = funcs[r.first->second];
    existing.usr = u.first;
    if (!tryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));
  }
}

void DB::update(const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, QueryType::Def>> &&us) {
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
    auto r = type_usr.try_emplace({u.first}, type_usr.size());
    if (r.second)
      types.emplace_back();
    QueryType &existing = types[r.first->second];
    existing.usr = u.first;
    if (!tryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));
  }
}

void DB::update(const Lid2file_id &lid2file_id, int file_id,
                std::vector<std::pair<Usr, QueryVar::Def>> &&us) {
  for (auto &u : us) {
    auto &def = u.second;
    assert(!std::string_view(def.detailed_name).empty());
    u.second.file_id = file_id;
    if (def.spell) {
      assignFileId(lid2file_id, file_id, *def.spell);
      files[def.spell->file_id].symbol2refcnt[{
          {def.spell->range, u.first, Kind::Var, def.spell->role},
          def.spell->extent}]++;
    }
    auto r = var_usr.try_emplace({u.first}, var_usr.size());
    if (r.second)
      vars.emplace_back();
    QueryVar &existing = vars[r.first->second];
    existing.usr = u.first;
    if (!tryReplaceDef(existing.def, std::move(def)))
      existing.def.push_back(std::move(def));
  }
}

std::string_view DB::getSymbolName(SymbolIdx sym, bool qualified) const {
  Usr usr = sym.usr;
  switch (sym.kind) {
  default:
    break;
  case Kind::File:
    if (files[usr].def)
      return files[usr].def->path;
    break;
  case Kind::Func:
    if (const auto *def = getFunc(usr).anyDef())
      return def->name(qualified);
    break;
  case Kind::Type:
    if (const auto *def = getType(usr).anyDef())
      return def->name(qualified);
    break;
  case Kind::Var:
    if (const auto *def = getVar(usr).anyDef())
      return def->name(qualified);
    break;
  }
  return "";
}

std::vector<uint8_t>
DB::getFileSet(const std::vector<std::string> &folders) const {
  if (folders.empty())
    return std::vector<uint8_t>(files.size(), 1);
  std::vector<uint8_t> file_set(files.size());
  for (auto &file : files)
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
  return file_set;
}

namespace {
// Computes roughly how long |range| is.
int computeRangeSize(const Range &range) {
  if (range.start.line != range.end.line)
    return INT_MAX;
  return range.end.column - range.start.column;
}

template <typename U, typename V, typename C>
std::vector<Use> getDeclarations(U &entity_usr, V &entities, const C &usrs) {
  std::vector<Use> ret;
  ret.reserve(usrs.size());
  for (Usr usr : usrs) {
    auto &entity = entities[entity_usr[{usr}]];
    bool has_def = false;
    for (auto &def : entity.def)
      if (def.spell) {
        ret.push_back(*def.spell);
        has_def = true;
        break;
      }
    if (!has_def && entity.declarations.size())
      ret.push_back(entity.declarations[0]);
  }
  return ret;
}
} // namespace

Maybe<DeclRef> getDefinitionSpell(DB *db, SymbolIdx sym) {
  Maybe<DeclRef> ret;
  eachEntityDef(db, sym, [&](const auto &def) { return !(ret = def.spell); });
  return ret;
}

std::vector<Use> getFuncDeclarations(DB *db,
                                     const db::scoped_vector<Usr> &usrs) {
  return getDeclarations(db->func_usr, db->funcs, usrs);
}
std::vector<Use> getTypeDeclarations(DB *db,
                                     const db::scoped_vector<Usr> &usrs) {
  return getDeclarations(db->type_usr, db->types, usrs);
}
std::vector<DeclRef>
getVarDeclarations(DB *db, const db::scoped_vector<Usr> &usrs, unsigned kind) {
  std::vector<DeclRef> ret;
  ret.reserve(usrs.size());
  for (Usr usr : usrs) {
    QueryVar &var = db->getVar(usr);
    bool has_def = false;
    for (auto &def : var.def)
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
    if (!has_def && var.declarations.size())
      ret.push_back(var.declarations[0]);
  }
  return ret;
}

std::vector<DeclRef> getNonDefDeclarations(DB *db, SymbolIdx sym) {
  switch (sym.kind) {
  case Kind::Func:
    return {db->getFunc(sym).declarations.begin(),
            db->getFunc(sym).declarations.end()};
  case Kind::Type:
    return {db->getType(sym).declarations.begin(),
            db->getType(sym).declarations.end()};
  case Kind::Var:
    return {db->getVar(sym).declarations.begin(),
            db->getVar(sym).declarations.end()};
  default:
    break;
  }
  return {};
}

std::vector<Use> getUsesForAllBases(DB *db, QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<QueryFunc *> stack{&root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    QueryFunc &func = *stack.back();
    stack.pop_back();
    if (auto *def = func.anyDef()) {
      eachDefinedFunc(db, def->bases, [&](QueryFunc &func1) {
        if (!seen.count(func1.usr)) {
          seen.insert(func1.usr);
          stack.push_back(&func1);
          ret.insert(ret.end(), func1.uses.begin(), func1.uses.end());
        }
      });
    }
  }

  return ret;
}

std::vector<Use> getUsesForAllDerived(DB *db, QueryFunc &root) {
  std::vector<Use> ret;
  std::vector<QueryFunc *> stack{&root};
  std::unordered_set<Usr> seen;
  seen.insert(root.usr);
  while (!stack.empty()) {
    QueryFunc &func = *stack.back();
    stack.pop_back();
    eachDefinedFunc(db, func.derived, [&](QueryFunc &func1) {
      if (!seen.count(func1.usr)) {
        seen.insert(func1.usr);
        stack.push_back(&func1);
        ret.insert(ret.end(), func1.uses.begin(), func1.uses.end());
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
  QueryFile &file = db->files[file_id];
  if (file.def) {
    *path = file.def->path;
    return DocumentUri::fromPath(*path);
  } else {
    *path = "";
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
      for (auto &def : entity.def) {
        ret = def.kind;
        break;
      }
    });
  }
  return ret;
}

std::optional<SymbolInformation> getSymbolInfo(const DB *db, SymbolIdx sym,
                                               bool detailed) {
  switch (sym.kind) {
  case Kind::Invalid:
    break;
  case Kind::File: {
    const QueryFile &file = db->getFile(sym);
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

std::vector<SymbolRef> findSymbolsAtLocation(WorkingFile *wfile,
                                             QueryFile *file, Position &ls_pos,
                                             bool smallest) {
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
