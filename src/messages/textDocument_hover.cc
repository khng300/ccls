// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"
#include "query.hh"

namespace ccls {
namespace {
struct MarkedString {
  std::optional<std::string> language;
  std::string value;
};
struct Hover {
  std::vector<MarkedString> contents;
  std::optional<lsRange> range;
};

void reflect(JsonWriter &vis, MarkedString &v) {
  // If there is a language, emit a `{language:string, value:string}` object. If
  // not, emit a string.
  if (v.language) {
    vis.startObject();
    REFLECT_MEMBER(language);
    REFLECT_MEMBER(value);
    vis.endObject();
  } else {
    reflect(vis, v.value);
  }
}
REFLECT_STRUCT(Hover, contents, range);

const char *languageIdentifier(LanguageId lang) {
  switch (lang) {
    // clang-format off
  case LanguageId::C: return "c";
  case LanguageId::Cpp: return "cpp";
  case LanguageId::ObjC: return "objective-c";
  case LanguageId::ObjCpp: return "objective-cpp";
  default: return "";
    // clang-format on
  }
}

// Returns the hover or detailed name for `sym`, if any.
std::pair<std::optional<MarkedString>, std::optional<MarkedString>> getHover(DB *db, LanguageId lang, SymbolRef sym,
                                                                             int file_id) {
  const char *comments = nullptr;
  std::optional<MarkedString> ls_comments, hover;
  withEntity(db, sym, [&](const auto &entity) {
    allOf(entity.defs(db), [&](const auto &d) {
      if (!comments && d.comments.size())
        comments = d.comments.data();
      if (d.spell) {
        if (d.comments.size())
          comments = d.comments.data();
        if (const char *s = d.hover.size()           ? d.hover.data()
                            : d.detailed_name.size() ? d.detailed_name.data()
                                                     : nullptr) {
          if (!hover)
            hover = {languageIdentifier(lang), s};
          else if (strlen(s) > hover->value.size())
            hover->value = s;
        }
        if (d.spell->file_id == file_id)
          return false;
      }
      return true;
    });
    if (auto c = entity.defs(db); !hover && c.size()) {
      auto &d = *c.begin();
      hover = {languageIdentifier(lang)};
      if (d.hover.size())
        hover->value = d.hover.data();
      else if (d.detailed_name.size())
        hover->value = d.detailed_name.data();
    }
    if (comments)
      ls_comments = MarkedString{std::nullopt, comments};
  });
  return {hover, ls_comments};
}
} // namespace

void MessageHandler::textDocument_hover(TextDocumentPositionParam &param, ReplyOnce &reply) {
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  auto [file, wf] = findOrFail(db, param.textDocument.uri.getPath(), reply);
  if (!wf)
    return;

  Hover result;
  for (SymbolRef sym : findSymbolsAtLocation(wf, &*file, param.position)) {
    std::optional<lsRange> ls_range = getLsRange(wfiles->getFile(file->def->path), sym.range);
    if (!ls_range)
      continue;

    auto [hover, comments] = getHover(db, file->def->language, sym, file->id);
    if (comments || hover) {
      result.range = *ls_range;
      if (comments)
        result.contents.push_back(*comments);
      if (hover)
        result.contents.push_back(*hover);
      break;
    }
  }

  reply(result);
}
} // namespace ccls
