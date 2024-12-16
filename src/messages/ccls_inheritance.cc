// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "hierarchy.hh"
#include "message_handler.hh"
#include "pipeline.hh"
#include "query.hh"

#include <unordered_set>

namespace ccls {
namespace {
struct Param : TextDocumentPositionParam {
  // If id+kind are specified, expand a node; otherwise textDocument+position
  // should be specified for building the root and |levels| of nodes below.
  Usr usr;
  std::string id;
  Kind kind = Kind::Invalid;

  // true: derived classes/functions; false: base classes/functions
  bool derived = false;
  bool qualified = true;
  int levels = 1;
  bool hierarchy = false;
};

REFLECT_STRUCT(Param, textDocument, position, id, kind, derived, qualified, levels, hierarchy);

struct Out_cclsInheritance {
  Usr usr;
  std::string id;
  Kind kind;
  std::string_view name;
  Location location;
  // For unexpanded nodes, this is an upper bound because some entities may be
  // undefined. If it is 0, there are no members.
  int numChildren;
  // Empty if the |levels| limit is reached.
  std::vector<Out_cclsInheritance> children;
};
REFLECT_STRUCT(Out_cclsInheritance, id, kind, name, location, numChildren, children);

bool expand(MessageHandler *m, DB *db, Out_cclsInheritance *entry, bool derived, bool qualified, int levels);

template <typename Q>
bool expandHelper(MessageHandler *m, DB *db, Out_cclsInheritance *entry, bool derived, bool qualified, int levels,
                  Q &&entity) {
  auto def = entity.anyDef(db);
  if (def) {
    entry->name = def->name(qualified);
    if (def->spell) {
      if (auto loc = getLsLocation(db, m->wfiles, *def->spell))
        entry->location = *loc;
    } else {
      auto decls = entity.decls(db);
      if (auto dr = decls.begin(); dr != decls.end())
        if (auto loc = getLsLocation(db, m->wfiles, *dr))
          entry->location = *loc;
    }
  } else if (!derived) {
    entry->numChildren = 0;
    return false;
  }
  std::unordered_set<Usr> seen;
  if (derived) {
    if (levels > 0) {
      forEach(entity.deriveds(db), [&](auto usr) {
        if (!seen.insert(usr).second)
          return;
        Out_cclsInheritance entry1;
        entry1.id = std::to_string(usr);
        entry1.usr = usr;
        entry1.kind = entry->kind;
        if (expand(m, db, &entry1, derived, qualified, levels - 1))
          entry->children.push_back(std::move(entry1));
      });
      entry->numChildren = int(entry->children.size());
    } else
      entry->numChildren = int(entity.deriveds(db).size());
  } else {
    if (levels > 0) {
      for (auto usr : def->bases) {
        if (!seen.insert(usr).second)
          continue;
        Out_cclsInheritance entry1;
        entry1.id = std::to_string(usr);
        entry1.usr = usr;
        entry1.kind = entry->kind;
        if (expand(m, db, &entry1, derived, qualified, levels - 1))
          entry->children.push_back(std::move(entry1));
      }
      entry->numChildren = int(entry->children.size());
    } else
      entry->numChildren = int(def->bases.size());
  }
  return true;
}

bool expand(MessageHandler *m, DB *db, Out_cclsInheritance *entry, bool derived, bool qualified, int levels) {
  if (entry->kind == Kind::Func)
    return expandHelper(m, db, entry, derived, qualified, levels, db->getFunc(entry->usr));
  else
    return expandHelper(m, db, entry, derived, qualified, levels, db->getType(entry->usr));
}

std::optional<Out_cclsInheritance> buildInitial(MessageHandler *m, DB *db, SymbolRef sym, bool derived, bool qualified,
                                                int levels) {
  Out_cclsInheritance entry;
  entry.id = std::to_string(sym.usr);
  entry.usr = sym.usr;
  entry.kind = sym.kind;
  expand(m, db, &entry, derived, qualified, levels);
  return entry;
}

void inheritance(MessageHandler *m, DB *db, Param &param, ReplyOnce &reply) {
  std::optional<Out_cclsInheritance> result;
  if (param.id.size()) {
    try {
      param.usr = std::stoull(param.id);
    } catch (...) {
      return;
    }
    result.emplace();
    result->id = std::to_string(param.usr);
    result->usr = param.usr;
    result->kind = param.kind;
    if (!(((param.kind == Kind::Func && db->hasFunc(param.usr)) ||
           (param.kind == Kind::Type && db->hasType(param.usr))) &&
          expand(m, db, &*result, param.derived, param.qualified, param.levels)))
      result.reset();
  } else {
    auto [file, wf] = m->findOrFail(db, param.textDocument.uri.getPath(), reply);
    if (!wf) {
      return;
    }
    for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, param.position))
      if (sym.kind == Kind::Func || sym.kind == Kind::Type) {
        result = buildInitial(m, db, sym, param.derived, param.qualified, param.levels);
        break;
      }
  }

  if (param.hierarchy)
    reply(result);
  else
    reply(flattenHierarchy(result));
}
} // namespace

void MessageHandler::ccls_inheritance(JsonReader &reader, ReplyOnce &reply) {
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  Param param;
  reflect(reader, param);
  inheritance(this, db, param, reply);
}

void MessageHandler::textDocument_implementation(TextDocumentPositionParam &param, ReplyOnce &reply) {
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  Param param1;
  param1.textDocument = param.textDocument;
  param1.position = param.position;
  param1.derived = true;
  inheritance(this, db, param1, reply);
}
} // namespace ccls
