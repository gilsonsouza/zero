/**
 * Platform-dependent definitions.
 * Don't use '//' for comments in this file as this file is used
 * by C compilers too (it will cause an error on Sun C compiler).
 */

/**
 * platform-independent settings.
 * these are generated by CMake from shore-config-env.h.cmake
 * and placed in build-folder/config.
 */
#ifdef SHORE_CONFIG_ENV_H_PATH /* used for SM_PAGESIZE, etc */
#include SHORE_CONFIG_ENV_H_PATH
#else
#include "shore-config-env.h"
#endif

/** common definitions for all platforms. */

/* Name of package */
#define PACKAGE "fbtree"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "hkimura@cs.brown.edu."

/* Define to the full name of this package. */
#define PACKAGE_NAME "Foster B-tree"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "Foster B-tree 1.0"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "fbtree"

/* Define to the home page for this package. */
#define PACKAGE_URL ""

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.0"

/* include tracing code? */
#define TRACECODE 0

/* Version number of package */
#define VERSION "1.0"

/* track return-code checking */
#define W_DEBUG_RC 0

#if (defined(_LARGEFILE_SOURCE) && defined(_FILE_OFFSET_BITS)) || defined(ARCH_LP64)
#define LARGEFILE_AWARE
#endif


#if TRACECODE==1
#define W_TRACE
#endif

/* Opposite to WORDS_BIGENDIAN. Check this after "#ifndef WORDS_BIGENDIAN"
   to make sure you do have a valid config.h in include path. */
#ifndef WORDS_BIGENDIAN
#define WORDS_LITTLEENDIAN
#endif // WORDS_BIGENDIAN
