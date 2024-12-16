// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "clang_tu.hh"
#include "hierarchy.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "query.hh"

#include <clang/AST/Type.h>
#include <llvm/ADT/DenseSet.h>

#include <unordered_set>

namespace ccls {
using namespace clang;

namespace {
struct Param : TextDocumentPositionParam {
  // If id is specified, expand a node; otherwise textDocument+position should
  // be specified for building the root and |levels| of nodes below.
  Usr usr;
  std::string id;

  bool qualified = false;
  int levels = 1;
  // If Kind::Func and the point is at a type, list member functions instead of
  // member variables.
  Kind kind = Kind::Var;
  bool hierarchy = false;
};

REFLECT_STRUCT(Param, textDocument, position, id, qualified, levels, kind, hierarchy);

struct Out_cclsMember {
  Usr usr;
  std::string id;
  std::string_view name;
  std::string fieldName;
  Location location;
  // For unexpanded nodes, this is an upper bound because some entities may be
  // undefined. If it is 0, there are no members.
  int numChildren = 0;
  // Empty if the |levels| limit is reached.
  std::vector<Out_cclsMember> children;
};
REFLECT_STRUCT(Out_cclsMember, id, name, fieldName, location, numChildren, children);

bool expand(MessageHandler *m, DB *db, Out_cclsMember *entry, bool qualified, int levels, Kind memberKind);

// Add a field to |entry| which is a Func/Type.
void doField(MessageHandler *m, DB *db, Out_cclsMember *entry, const QueryVar &var, int64_t offset, bool qualified,
             int levels) {
  auto def1 = var.anyDef(db);
  if (!def1)
    return;
  Out_cclsMember entry1;
  // With multiple inheritance, the offset is incorrect.
  if (offset >= 0) {
    if (offset / 8 < 10)
      entry1.fieldName += ' ';
    entry1.fieldName += std::to_string(offset / 8);
    if (offset % 8) {
      entry1.fieldName += '.';
      entry1.fieldName += std::to_string(offset % 8);
    }
    entry1.fieldName += ' ';
  }
  if (qualified)
    entry1.fieldName += def1->detailed_name;
  else {
    entry1.fieldName += def1->detailed_name.view().substr(0, def1->qual_name_offset);
    entry1.fieldName += def1->name(false);
  }
  if (def1->spell) {
    if (std::optional<Location> loc = getLsLocation(db, m->wfiles, *def1->spell))
      entry1.location = *loc;
  }
  if (def1->type) {
    entry1.id = std::to_string(def1->type);
    entry1.usr = def1->type;
    if (expand(m, db, &entry1, qualified, levels, Kind::Var))
      entry->children.push_back(std::move(entry1));
  } else {
    entry1.id = "0";
    entry1.usr = 0;
    entry->children.push_back(std::move(entry1));
  }
}

// Expand a type node by adding members recursively to it.
bool expand(MessageHandler *m, DB *db, Out_cclsMember *entry, bool qualified, int levels, Kind memberKind) {
  if (0 < entry->usr && entry->usr <= BuiltinType::LastKind) {
    entry->name = clangBuiltinTypeName(int(entry->usr));
    return true;
  }
  const QueryType *type = &db->getType(entry->usr);
  auto def = type->anyDef(db);
  // builtin types have no declaration and empty |qualified|.
  if (!def)
    return false;
  entry->name = def->name(qualified);
  std::unordered_set<Usr> seen;
  if (levels > 0) {
    std::vector<const QueryType *> stack;
    seen.insert(type->usr);
    stack.push_back(type);
    while (stack.size()) {
      type = stack.back();
      stack.pop_back();
      auto def = type->anyDef(db);
      if (!def)
        continue;
      if (def->kind != SymbolKind::Namespace)
        for (Usr usr : def->bases) {
          const auto &type1 = db->getType(usr);
          if (type1.defs(db).size()) {
            seen.insert(type1.usr);
            stack.push_back(&type1);
          }
        }
      if (def->alias_of) {
        const QueryType &type1 = db->getType(def->alias_of);
        auto def1 = type1.anyDef(db);
        Out_cclsMember entry1;
        entry1.id = std::to_string(def->alias_of);
        entry1.usr = def->alias_of;
        if (def1 && def1->spell) {
          // The declaration of target type.
          if (std::optional<Location> loc = getLsLocation(db, m->wfiles, *def1->spell))
            entry1.location = *loc;
        } else if (def->spell) {
          // Builtin types have no declaration but the typedef declaration
          // itself is useful.
          if (std::optional<Location> loc = getLsLocation(db, m->wfiles, *def->spell))
            entry1.location = *loc;
        }
        if (def1 && qualified)
          entry1.fieldName = def1->detailed_name;
        if (expand(m, db, &entry1, qualified, levels - 1, memberKind)) {
          // For builtin types |name| is set.
          if (entry1.fieldName.empty())
            entry1.fieldName = std::string(entry1.name);
          entry->children.push_back(std::move(entry1));
        }
      } else if (memberKind == Kind::Func) {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        forEach(type->defs(db), [&](const auto &def) {
          for (Usr usr : def.funcs)
            if (seen1.insert(usr).second) {
              const QueryFunc &func1 = db->getFunc(usr);
              if (auto def1 = func1.anyDef(db)) {
                Out_cclsMember entry1;
                entry1.fieldName = def1->name(false);
                if (def1->spell) {
                  if (auto loc = getLsLocation(db, m->wfiles, *def1->spell))
                    entry1.location = *loc;
                } else {
                  auto decls = func1.decls(db);
                  auto dr = decls.begin();
                  if (dr != decls.end())
                    if (auto loc = getLsLocation(db, m->wfiles, *dr))
                      entry1.location = *loc;
                }
                entry->children.push_back(std::move(entry1));
              }
            }
        });
      } else if (memberKind == Kind::Type) {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        forEach(type->defs(db), [&](const auto &def) {
          for (Usr usr : def.types)
            if (seen1.insert(usr).second) {
              const QueryType &type1 = db->getType(usr);
              if (auto def1 = type1.anyDef(db)) {
                Out_cclsMember entry1;
                entry1.fieldName = def1->name(false);
                if (def1->spell) {
                  if (auto loc = getLsLocation(db, m->wfiles, *def1->spell))
                    entry1.location = *loc;
                } else {
                  auto decls = type1.decls(db);
                  auto dr = decls.begin();
                  if (dr != decls.end())
                    if (auto loc = getLsLocation(db, m->wfiles, *dr))
                      entry1.location = *loc;
                }
                entry->children.push_back(std::move(entry1));
              }
            }
        });
      } else {
        llvm::DenseSet<Usr, DenseMapInfoForUsr> seen1;
        forEach(type->defs(db), [&](const auto &def) {
          for (auto it : def.vars)
            if (seen1.insert(it.first).second) {
              const QueryVar &var = db->getVar(it.first);
              if (var.defs(db).size() != 0)
                doField(m, db, entry, var, it.second, qualified, levels - 1);
            }
        });
      }
    }
    entry->numChildren = int(entry->children.size());
  } else
    entry->numChildren = def->alias_of ? 1 : int(def->vars.size());
  return true;
}

std::optional<Out_cclsMember> buildInitial(MessageHandler *m, DB *db, Kind kind, Usr root_usr, bool qualified,
                                           int levels, Kind memberKind) {
  switch (kind) {
  default:
    return {};
  case Kind::Func: {
    const QueryFunc &func = db->getFunc(root_usr);
    auto def = func.anyDef(db);
    if (!def)
      return {};

    Out_cclsMember entry;
    // Not type, |id| is invalid.
    entry.name = def->name(qualified);
    if (def->spell) {
      if (auto loc = getLsLocation(db, m->wfiles, *def->spell))
        entry.location = *loc;
    }
    for (Usr usr : def->vars) {
      const QueryVar &var = db->getVar(usr);
      if (var.defs(db).size() != 0)
        doField(m, db, &entry, var, -1, qualified, levels - 1);
    }
    return entry;
  }
  case Kind::Type: {
    const QueryType &type = db->getType(root_usr);
    auto def = type.anyDef(db);
    if (!def)
      return {};

    Out_cclsMember entry;
    entry.id = std::to_string(root_usr);
    entry.usr = root_usr;
    if (def->spell) {
      if (auto loc = getLsLocation(db, m->wfiles, *def->spell))
        entry.location = *loc;
    }
    expand(m, db, &entry, qualified, levels, memberKind);
    return entry;
  }
  }
}
} // namespace

void MessageHandler::ccls_member(JsonReader &reader, ReplyOnce &reply) {
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  Param param;
  reflect(reader, param);
  std::optional<Out_cclsMember> result;
  if (param.id.size()) {
    try {
      param.usr = std::stoull(param.id);
    } catch (...) {
      return;
    }
    result.emplace();
    result->id = std::to_string(param.usr);
    result->usr = param.usr;
    // entry.name is empty as it is known by the client.
    if (!(db->hasType(param.usr) && expand(this, db, &*result, param.qualified, param.levels, param.kind)))
      result.reset();
  } else {
    auto [file, wf] = findOrFail(db, param.textDocument.uri.getPath(), reply);
    if (!wf)
      return;
    for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, param.position)) {
      switch (sym.kind) {
      case Kind::Func:
      case Kind::Type:
        result = buildInitial(this, db, sym.kind, sym.usr, param.qualified, param.levels, param.kind);
        break;
      case Kind::Var: {
        const QueryVar &var = db->getVar(sym);
        auto def = var.anyDef(db);
        if (def && def->type)
          result = buildInitial(this, db, Kind::Type, def->type, param.qualified, param.levels, param.kind);
        break;
      }
      default:
        continue;
      }
      break;
    }
  }

  if (param.hierarchy)
    reply(result);
  else
    reply(flattenHierarchy(result));
}
} // namespace ccls
