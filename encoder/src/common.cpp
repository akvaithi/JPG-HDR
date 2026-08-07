#include "common.h"

#include <cstdarg>

namespace iso21496 {

bool g_verbose = false;

void logf(const char* fmt, ...) {
  if (!g_verbose) return;
  va_list ap;
  va_start(ap, fmt);
  std::fputs("[iso21496] ", stderr);
  std::vfprintf(stderr, fmt, ap);
  std::fputc('\n', stderr);
  va_end(ap);
}

Bytes readFile(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) fail("cannot open input file: " + path);
  std::fseek(f, 0, SEEK_END);
  long size = std::ftell(f);
  if (size < 0) {
    std::fclose(f);
    fail("cannot determine size of: " + path);
  }
  std::fseek(f, 0, SEEK_SET);
  Bytes data(static_cast<size_t>(size));
  size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
  std::fclose(f);
  if (got != data.size()) fail("short read on: " + path);
  return data;
}

void writeFile(const std::string& path, const Bytes& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) fail("cannot open output file for writing: " + path);
  size_t put = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
  int flushed = std::fflush(f);
  std::fclose(f);
  if (put != data.size() || flushed != 0) fail("short write on: " + path);
}

}  // namespace iso21496
