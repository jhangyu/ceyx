#ifndef CEYX_UTF8_PATH_H
#define CEYX_UTF8_PATH_H

// Every path crosses the FFI as UTF-8 (Dart's toNativeUtf8). POSIX file APIs
// take UTF-8 as-is, but on Windows the narrow CRT/SDK APIs (fopen,
// LibRaw::open_file(const char*), the DNG SDK's dng_file_stream) decode it in
// the ANSI code page, so any non-ASCII path -- a folder named in Chinese, an
// accented file name -- fails to open. Every narrow open goes through here,
// so the platform difference lives in one place. (libheif's read_from_file
// already handles UTF-8 on Windows and is left alone.)

#include <cstdio>

#ifdef _WIN32
#include <filesystem>
#include <string>

// UTF-16 form of a UTF-8 path, for the wide-char Windows APIs. Invalid UTF-8
// yields an empty string -- the caller's open then fails the ordinary way --
// because callers sit behind extern "C" entry points no exception may cross.
inline std::wstring ceyx_utf8_to_wide(const char *utf8_path) {
  try {
    return std::filesystem::u8path(utf8_path).wstring();
  } catch (...) {
    return std::wstring();
  }
}
#endif

inline std::FILE *ceyx_fopen_utf8(const char *utf8_path, const char *mode) {
#ifdef _WIN32
  const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
  return _wfopen(ceyx_utf8_to_wide(utf8_path).c_str(), wide_mode.c_str());
#else
  return std::fopen(utf8_path, mode);
#endif
}

#endif  // CEYX_UTF8_PATH_H
