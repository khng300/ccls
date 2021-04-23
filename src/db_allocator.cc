#include "db_allocator.hh"
#include "utils.hh"

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>

namespace ccls {
namespace db {
namespace impl {

static constexpr size_t DatabaseMapSize = size_t(128) * 1024 * 1024 * 1024;

managed_mapped_file createManagedMappedFile(const std::string &db_dir) {
  std::string path = db_dir;
  ensureEndsInSlash(path);
  path += "ccls_mmaped_db";
  return managed_mapped_file(boost::interprocess::open_or_create, path.c_str(),
                             DatabaseMapSize);
}

} // namespace impl
} // namespace db
} // namespace ccls