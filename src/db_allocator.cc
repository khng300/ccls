#include "db_allocator.hh"
#include "utils.hh"

#include <random>

#include <boost/interprocess/managed_mapped_file.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Process.h>

namespace ccls {
namespace db {
namespace impl {

managed_mapped_file createManagedMappedFile(const std::string &db_dir) {
  std::string path = db_dir;
  path += "ccls_mmaped_db.";
  if (auto env = getenv("CCLS_DB_NAME"); env != nullptr && env[0]) {
    path += env;
  } else {
    std::random_device rd;
    std::mt19937 gen(rd());
    path += std::to_string(llvm::sys::Process::getProcessId()) + "." +
            std::to_string(
                std::uniform_int_distribution<uint32_t>(0, UINT32_MAX)(gen));
  }
  return managed_mapped_file(boost::interprocess::open_or_create, path.c_str(),
                             64ull * 1024 * 1024 * 1024);
}

} // namespace impl
} // namespace db
} // namespace ccls