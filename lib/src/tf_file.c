// Maps a file so the parser reads it in place. Mapped rather than read: the pages
// are file-backed, so a large input costs address space, not committed memory,
// and the byte offsets the sink is handed stay valid. The 4 GiB ceiling is the
// `uint32_t` offset, as in tree-sitter.
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "tree_feller.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static bool tf_file__fail(TFError *error, const char *path, const char *what, const char *why) {
  if (error) {
    *error = (TFError){0};
    snprintf(error->message, TF_ERROR_MESSAGE_SIZE, "%s: %s: %s", path, what, why);
  }
  return false;
}

static bool tf_file__too_large(TFError *error, const char *path) {
  return tf_file__fail(error, path, "cannot map",
                       "larger than the 4 GiB a byte offset can address");
}

bool tf_file_open(TFFile *self, const char *path, TFError *error) {
  *self = (TFFile){0};

#ifdef _WIN32
  // CreateFileMapping rather than a read into a buffer: a private copy of the
  // file would undo the point of the exercise on the largest inputs.
  char why[128];
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    snprintf(why, sizeof(why), "error %lu", (unsigned long)GetLastError());
    return tf_file__fail(error, path, "cannot open", why);
  }

  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size)) {
    snprintf(why, sizeof(why), "error %lu", (unsigned long)GetLastError());
    CloseHandle(file);
    return tf_file__fail(error, path, "cannot size", why);
  }
  if ((unsigned long long)size.QuadPart > UINT32_MAX) {
    CloseHandle(file);
    return tf_file__too_large(error, path);
  }
  self->size = (uint32_t)size.QuadPart;
  if (self->size == 0) {
    CloseHandle(file);
    self->data = "";
    return true;
  }

  HANDLE mapping = CreateFileMappingA(file, NULL, PAGE_READONLY, 0, 0, NULL);
  CloseHandle(file);
  if (!mapping) {
    snprintf(why, sizeof(why), "error %lu", (unsigned long)GetLastError());
    return tf_file__fail(error, path, "cannot map", why);
  }
  void *data = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  CloseHandle(mapping);  // the view holds its own reference
  if (!data) {
    snprintf(why, sizeof(why), "error %lu", (unsigned long)GetLastError());
    return tf_file__fail(error, path, "cannot map", why);
  }
  self->data = data;
  return true;
#else
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return tf_file__fail(error, path, "cannot open", strerror(errno));
  }

  struct stat info;
  if (fstat(fd, &info) != 0) {
    int saved = errno;
    close(fd);
    return tf_file__fail(error, path, "cannot stat", strerror(saved));
  }
  if ((unsigned long long)info.st_size > UINT32_MAX) {
    close(fd);
    return tf_file__too_large(error, path);
  }
  self->size = (uint32_t)info.st_size;
  if (self->size == 0) {
    // mmap rejects a zero length, and an empty file is a valid parse.
    close(fd);
    self->data = "";
    return true;
  }

  void *data = mmap(NULL, self->size, PROT_READ, MAP_PRIVATE, fd, 0);
  int saved = errno;
  close(fd);
  if (data == MAP_FAILED) {
    return tf_file__fail(error, path, "cannot map", strerror(saved));
  }
  self->data = data;
  return true;
#endif
}

void tf_file_close(TFFile *self) {
  // An empty file is "" and was never mapped; a failed map leaves `size` set but
  // no `data`.
  if (self->size > 0 && self->data) {
#ifdef _WIN32
    UnmapViewOfFile(self->data);
#else
    munmap((void *)self->data, self->size);
#endif
  }
  *self = (TFFile){0};
}
