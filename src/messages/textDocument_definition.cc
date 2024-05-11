// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"
#include "query.hh"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>

namespace ccls {
void MessageHandler::textDocument_declaration(TextDocumentPositionParam &param,
                                              ReplyOnce &reply) {
  auto txn = TxnDB::begin(qs, true);
  auto db = txn.db();
  int file_id;
  auto [file, wf] =
      findOrFail(db, param.textDocument.uri.getPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Position &ls_pos = param.position;
  for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, param.position))
    for (DeclRef dr : getNonDefDeclarations(db, sym))
      if (!(dr.file_id == file_id &&
            dr.range.contains(ls_pos.line, ls_pos.character)))
        if (auto loc = getLocationLink(db, wfiles, dr))
          result.push_back(loc);
  reply.replyLocationLink(result);
}

void MessageHandler::textDocument_definition(TextDocumentPositionParam &param,
                                             ReplyOnce &reply) {
  auto txn = TxnDB::begin(qs, true);
  auto db = txn.db();
  int file_id;
  auto [file, wf] =
      findOrFail(db, param.textDocument.uri.getPath(), reply, &file_id);
  if (!wf)
    return;

  std::vector<LocationLink> result;
  Maybe<DeclRef> on_def;
  Position &ls_pos = param.position;

  for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, ls_pos, true)) {
    // Special cases which are handled:
    //  - symbol has declaration but no definition (ie, pure virtual)
    //  - goto declaration while in definition of recursive type
    std::vector<DeclRef> drs;
    eachEntityDef(db, sym, [&](const auto &def) {
      if (def.spell) {
        DeclRef spell = *def.spell;
        if (spell.file_id == file_id &&
            spell.range.contains(ls_pos.line, ls_pos.character)) {
          on_def = spell;
          drs.clear();
          return false;
        }
        drs.push_back(spell);
      }
      return true;
    });

    // |uses| is empty if on a declaration/definition, otherwise it includes
    // all declarations/definitions.
    if (drs.empty()) {
      for (DeclRef dr : getNonDefDeclarations(db, sym))
        if (!(dr.file_id == file_id &&
              dr.range.contains(ls_pos.line, ls_pos.character)))
          drs.push_back(dr);
      // There is no declaration but the cursor is on a definition.
      if (drs.empty() && on_def)
        drs.push_back(*on_def);
    }
    for (DeclRef dr : drs)
      if (auto loc = getLocationLink(db, wfiles, dr))
        result.push_back(loc);
  }

  if (result.empty()) {
    Maybe<Range> range;
    // Check #include
    for (const QueryFile::Def::IndexInclude &include : file->def->includes) {
      if (include.line == ls_pos.line) {
        result.push_back(
            {DocumentUri::fromPath(include.resolved_path).raw_uri});
        range = {{0, 0}, {0, 0}};
        break;
      }
    }
    // Find the best match of the identifier at point.
    if (!range) {
      Position position = param.position;
      const std::string &buffer = wf->buffer_content;
      std::string_view query = lexIdentifierAroundPos(position, buffer);
      std::string_view short_query = query;
      {
        auto pos = query.rfind(':');
        if (pos != std::string::npos)
          short_query = query.substr(pos + 1);
      }

      // For symbols whose short/detailed names contain |query| as a
      // substring, we use the tuple <length difference, negative position,
      // not in the same file, line distance> to find the best match.
      std::tuple<int, int, bool, int> best_score{INT_MAX, 0, true, 0};
      SymbolIdx best_sym;
      best_sym.kind = Kind::Invalid;
      auto fn = [&](SymbolIdx sym) {
        std::string short_name = db->getSymbolName(sym, false),
                    name = short_query.size() < query.size()
                               ? db->getSymbolName(sym, true)
                               : short_name;
        if (short_name != short_query)
          return;
        if (Maybe<DeclRef> dr = getDefinitionSpell(db, sym)) {
          std::tuple<int, int, bool, int> score{
              int(name.size() - short_query.size()), 0, dr->file_id != file_id,
              std::abs(dr->range.start.line - position.line)};
          // Update the score with qualified name if the qualified name
          // occurs in |name|.
          auto pos = name.rfind(query);
          if (pos != std::string::npos) {
            std::get<0>(score) = int(name.size() - query.size());
            std::get<1>(score) = -int(pos);
          }
          if (score < best_score) {
            best_score = score;
            best_sym = sym;
          }
        }
      };
      db->allOfUsr(Kind::Func, [&](const auto id) {
        const auto &func = db->id2Func(id);
        fn({func.usr, Kind::Func});
        return true;
      });
      db->allOfUsr(Kind::Type, [&](const auto id) {
        const auto &type = db->id2Type(id);
        fn({type.usr, Kind::Type});
        return true;
      });
      db->allOfUsr(Kind::Var, [&](const auto id) {
        const auto &var = db->id2Var(id);
        fn({var.usr, Kind::Var});
        return true;
      });

      if (best_sym.kind != Kind::Invalid) {
        Maybe<DeclRef> dr = getDefinitionSpell(db, best_sym);
        assert(dr);
        if (auto loc = getLocationLink(db, wfiles, *dr))
          result.push_back(loc);
      }
    }
  }

  reply.replyLocationLink(result);
}

void MessageHandler::textDocument_typeDefinition(
    TextDocumentPositionParam &param, ReplyOnce &reply) {
  auto txn = TxnDB::begin(qs, true);
  auto db = txn.db();
  auto [file, wf] = findOrFail(db, param.textDocument.uri.getPath(), reply);
  if (!file)
    return;

  std::vector<LocationLink> result;
  auto add = [&](const QueryType &type) {
    allOf(type.defCursor(*db), [&](const auto &def) {
      if (def.spell)
        if (auto loc = getLocationLink(db, wfiles, *def.spell))
          result.push_back(loc);
      return true;
    });
    if (result.empty()) {
      allOf(type.declCursor(*db), [&](auto dr) {
        if (auto loc = getLocationLink(db, wfiles, dr))
          result.push_back(loc);
        return true;
      });
    }
  };
  for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, param.position)) {
    switch (sym.kind) {
    case Kind::Var: {
      const QueryVar &var = db->getVar(sym);
      auto def = var.getAnyDef(*db);
      if (def && def->type)
        add(db->getType(def->type));
      break;
    }
    case Kind::Type: {
      const QueryType &type = db->getType(sym);
      allOf(type.defCursor(*db), [&](const auto &def) {
        if (def.alias_of) {
          add(db->getType(def.alias_of));
          return false;
        }
        return true;
      });
      break;
    }
    default:
      break;
    }
  }

  reply.replyLocationLink(result);
}
} // namespace ccls
