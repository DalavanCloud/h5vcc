// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/os_compat_android.h"

#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time64.h>

#include "base/rand_util.h"
#include "base/string_piece.h"
#include "base/stringprintf.h"

// There is no futimes() avaiable in Bionic, so we provide our own
// implementation until it is there.
extern "C" {

int futimes(int fd, const struct timeval tv[2]) {
  const std::string fd_path = StringPrintf("/proc/self/fd/%d", fd);
  return utimes(fd_path.c_str(), tv);
}

// Android has only timegm64() and no timegm().
// We replicate the behaviour of timegm() when the result overflows time_t.
time_t timegm(struct tm* const t) {
  // time_t is signed on Android.
  static const time_t kTimeMax = ~(1 << (sizeof(time_t) * CHAR_BIT - 1));
  static const time_t kTimeMin = (1 << (sizeof(time_t) * CHAR_BIT - 1));
  time64_t result = timegm64(t);
  if (result < kTimeMin || result > kTimeMax)
    return -1;
  return result;
}

// The following is only needed when building with GCC 4.6 or higher
// (i.e. not with Android GCC 4.4.3, nor with Clang).
//
// GCC is now capable of optimizing successive calls to sin() and cos() into
// a single call to sincos(). This means that source code that looks like:
//
//     double c, s;
//     c = cos(angle);
//     s = sin(angle);
//
// Will generate machine code that looks like:
//
//     double c, s;
//     sincos(angle, &s, &c);
//
// Unfortunately, sincos() and friends are not part of the Android libm.so
// library provided by the NDK for API level 9. When the optimization kicks
// in, it makes the final build fail with a puzzling message (puzzling
// because 'sincos' doesn't appear anywhere in the sources!).
//
// To solve this, we provide our own implementation of the sincos() function
// and related friends. Note that we must also explicitely tell GCC to disable
// optimizations when generating these. Otherwise, the generated machine code
// for each function would simply end up calling itself, resulting in a
// runtime crash due to stack overflow.
//
#if defined(__GNUC__) && !defined(__clang__)

// For the record, Clang does not support the 'optimize' attribute.
// In the unlikely event that it begins performing this optimization too,
// we'll have to find a different way to achieve this. NOTE: Tested with O1
// which still performs the optimization.
//
#define GCC_NO_OPTIMIZE  __attribute__((optimize("O0")))

GCC_NO_OPTIMIZE
void sincos(double angle, double* s, double *c) {
  *c = cos(angle);
  *s = sin(angle);
}

GCC_NO_OPTIMIZE
void sincosf(float angle, float* s, float* c) {
  *c = cosf(angle);
  *s = sinf(angle);
}

#endif // __GNUC__ && !__clang__

// An implementation of mkdtemp, since it is not exposed by the NDK
// for native API level 9 that we target.
//
// For any changes in the mkdtemp function, you should manually run the unittest
// OsCompatAndroidTest.DISABLED_TestMkdTemp in your local machine to check if it
// passes. Please don't enable it, since it creates a directory and may be
// source of flakyness.
char* mkdtemp(char* path) {
  if (path == NULL) {
    errno = EINVAL;
    return NULL;
  }

  const int path_len = strlen(path);

  // The last six characters of 'path' must be XXXXXX.
  const base::StringPiece kSuffix("XXXXXX");
  const int kSuffixLen = kSuffix.length();
  if (!base::StringPiece(path, path_len).ends_with(kSuffix)) {
    errno = EINVAL;
    return NULL;
  }

  // If the path contains a directory, as in /tmp/foo/XXXXXXXX, make sure
  // that /tmp/foo exists, otherwise we're going to loop a really long
  // time for nothing below
  char* dirsep = strrchr(path, '/');
  if (dirsep != NULL) {
    struct stat st;
    int ret;

    *dirsep = '\0';  // Terminating directory path temporarily

    ret = stat(path, &st);

    *dirsep = '/';  // Restoring directory separator
    if (ret < 0)  // Directory probably does not exist
      return NULL;
    if (!S_ISDIR(st.st_mode)) {  // Not a directory
      errno = ENOTDIR;
      return NULL;
    }
  }

  // Max number of tries using different random suffixes.
  const int kMaxTries = 100;

  // Now loop until we CAN create a directory by that name or we reach the max
  // number of tries.
  for (int i = 0; i < kMaxTries; ++i) {
    // Fill the suffix XXXXXX with a random string composed of a-z chars.
    for (int pos = 0; pos < kSuffixLen; ++pos) {
      char rand_char = static_cast<char>(base::RandInt('a', 'z'));
      path[path_len - kSuffixLen + pos] = rand_char;
    }
    if (mkdir(path, 0700) == 0) {
      // We just created the directory succesfully.
      return path;
    }
    if (errno != EEXIST) {
      // The directory doesn't exist, but an error occured
      return NULL;
    }
  }

  // We reached the max number of tries.
  return NULL;
}

}  // extern "C"
