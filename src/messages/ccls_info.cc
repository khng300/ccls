// Copyright 2017-2018 ccls Authors
// SPDX-License-Identifier: Apache-2.0

#include "message_handler.hh"
#include "pipeline.hh"
#include "project.hh"
#include "query.hh"

namespace ccls {
REFLECT_STRUCT(IndexInclude, line, resolved_path);
REFLECT_STRUCT(FileDef::IndexInclude, line, resolved_path);
REFLECT_STRUCT(FileDef, path, args, language, dependencies, includes, skipped_ranges);

namespace {
struct Out_cclsInfo {
  struct DB {
    int files, funcs, types, vars;
  } db;
  struct Pipeline {
    int64_t lastIdle, completed, enqueued, opened;
  } pipeline;
  struct Project {
    int entries;
  } project;
};
REFLECT_STRUCT(Out_cclsInfo::DB, files, funcs, types, vars);
REFLECT_STRUCT(Out_cclsInfo::Pipeline, lastIdle, completed, enqueued, opened);
REFLECT_STRUCT(Out_cclsInfo::Project, entries);
REFLECT_STRUCT(Out_cclsInfo, db, pipeline, project);
} // namespace

void MessageHandler::ccls_info(EmptyParam &, ReplyOnce &reply) {
  Out_cclsInfo result;
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  result.db.files = db->files().size();
  result.db.funcs = db->allUsrs(Kind::Func).size();
  result.db.types = db->allUsrs(Kind::Type).size();
  result.db.vars = db->allUsrs(Kind::Var).size();
  result.pipeline.lastIdle = pipeline::stats.last_idle;
  result.pipeline.completed = pipeline::stats.completed;
  result.pipeline.enqueued = pipeline::stats.enqueued;
  result.pipeline.opened = pipeline::stats.opened;
  result.project.entries = 0;
  for (auto &[_, folder] : project->root2folder)
    result.project.entries += folder.entries.size();
  reply(result);
}

struct FileInfoParam : TextDocumentParam {
  bool dependencies = false;
  bool includes = false;
  bool skipped_ranges = false;
};
REFLECT_STRUCT(FileInfoParam, textDocument, dependencies, includes, skipped_ranges);

void MessageHandler::ccls_fileInfo(JsonReader &reader, ReplyOnce &reply) {
  auto txn = TxnManager::begin(qs, true);
  auto db = txn.db();
  FileInfoParam param;
  reflect(reader, param);
  auto file = findFile(db, param.textDocument.uri.getPath());
  if (!file)
    return;

  FileDef result;
  const QueryFile::Def &o = *file->def;
  // Expose some fields of |QueryFile::Def|.
  result.path = o.path;
  std::for_each(o.args.begin(), o.args.end(), [&result](const auto &m) { result.args.emplace_back(m.data()); });
  result.language = o.language;
  if (param.includes)
    for (const auto &m : o.includes) {
      [&result](const auto &m) {
        FileDef::IndexInclude def;
        def.line = m.line;
        def.resolved_path = m.resolved_path.data();
        result.includes.emplace_back(def);
      }(m);
    }
  if (param.skipped_ranges)
    std::for_each(o.skipped_ranges.begin(), o.skipped_ranges.end(),
                  [&result](const auto &m) { result.skipped_ranges.emplace_back(m); });
  if (param.dependencies)
    std::for_each(o.dependencies.begin(), o.dependencies.end(),
                  [&result](const auto &m) { result.dependencies.emplace_back(m.data()); });
  reply(result);
}
} // namespace ccls
