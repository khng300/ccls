#include "db_allocator.hh"
#include "utils.hh"

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>

#include <random>

namespace ccls {
namespace db {
namespace impl {

managed_mapped_file createManagedMappedFile(const std::string &db_dir) {
  std::string path = db_dir;
  path += "ccls_mmap_db";
  if (auto env = getenv("CCLS_DB_NAME"); env != nullptr && env[0])
    path += env;
  return managed_mapped_file(boost::interprocess::open_or_create, path.c_str(),
                             64ull * 1024 * 1024 * 1024);
}

} // namespace impl
} // namespace db
} // namespace ccls