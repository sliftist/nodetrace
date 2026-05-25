#include "trace-writer.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cassert>

namespace v8 {
namespace internal {

TraceWriter::TraceWriter()
    : buf_(nullptr),
      ptr_(nullptr),
      buf_end_(nullptr),
      fd_(-1),
      cache_head_(0),
      cache_count_(0) {
  memset(cache_, 0, sizeof(cache_));
}

TraceWriter::~TraceWriter() {
  Flush();
  if (fd_ >= 0) {
    close(fd_);
    fd_ = -1;
  }
  if (buf_) {
    munmap(buf_, kTraceChunkSize);
    buf_ = nullptr;
  }
}

bool TraceWriter::Initialize(const char* output_path) {
  fd_ = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd_ < 0) {
    perror("TraceWriter: open");
    return false;
  }

  // Allocate the write buffer using MAP_ANONYMOUS so physical pages are only
  // committed as they are touched (lazy allocation).
  void* mem = mmap(nullptr, kTraceChunkSize,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
  if (mem == MAP_FAILED) {
    perror("TraceWriter: mmap");
    close(fd_);
    fd_ = -1;
    return false;
  }

  buf_     = static_cast<uint8_t*>(mem);
  ptr_     = buf_;
  buf_end_ = buf_ + kTraceChunkSize;
  return true;
}

void TraceWriter::FlushBuffer() {
  if (!buf_ || fd_ < 0) return;

  size_t used = static_cast<size_t>(ptr_ - buf_);
  if (used == 0) return;

  // Write the entire filled portion.  A single write() on Linux for a local
  // filesystem will not partially complete for sizes this large, but handle
  // it just in case.
  uint8_t* p   = buf_;
  size_t   rem = used;
  while (rem > 0) {
    ssize_t n = write(fd_, p, rem);
    if (n <= 0) {
      perror("TraceWriter: write");
      break;
    }
    p   += n;
    rem -= (size_t)n;
  }

  // Reset the write pointer to the beginning of the buffer.
  ptr_ = buf_;
}

void TraceWriter::Flush() {
  FlushBuffer();
}

}  // namespace internal
}  // namespace v8
