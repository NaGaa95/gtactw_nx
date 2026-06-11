/* imports.c -- .so import resolution
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * CTW 4.4.243: the table serves both libGame.so and the C++ runtime donor
 * (the APK's own libopenal.so). C++ runtime symbols (std::*, __cxa_*) are
 * NOT here -- they resolve module-to-module from the donor. The table takes
 * priority during resolution (see so_resolve_symbol), so the game's
 * OpenAL and mpg123 imports land on the native libraries instead of the
 * donor's dead OpenSLES backend.
 */

#define _GNU_SOURCE // vasprintf and friends

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/reent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <mpg123.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "movie_player.h"

extern uintptr_t __cxa_atexit;

extern uintptr_t __stack_chk_fail;

static char *__ctype_ = (char *)&_ctype_;

static uint64_t __stack_chk_guard_fake = 0x4242424242424242;

FILE *stderr_fake = (FILE *)0x1337;

// OpenAL hooks living in hooks/openal.c (frequency override + device capture)
extern ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused);
extern ALCdevice *alcOpenDeviceHook(const char *name);

void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assertion failed:\n%s:%d (%s): %s\n", file, line, func, expr);
  assert(0);
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
#ifdef DEBUG_LOG
  va_list list;
  static char string[0x1000];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

int __android_log_write(int prio, const char *tag, const char *text) {
  debugPrintf("%s: %s\n", tag, text);
  return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
#ifdef DEBUG_LOG
  static char string[0x1000];
  vsnprintf(string, sizeof(string), fmt, va);
  debugPrintf("%s: %s\n", tag, string);
#endif
  return 0;
}

// pthread stuff
// have to wrap it since struct sizes are different

int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;

  const int recursive = (mutexattr && *mutexattr == 1);
  *m = recursive ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER : PTHREAD_MUTEX_INITIALIZER;

  int ret = pthread_mutex_init(m, NULL);
  if (ret < 0) {
    free(m);
    return -1;
  }

  *uid = m;

  return 0;
}

int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}

int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}

int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = 0;
  if (!*uid) {
    ret = pthread_mutex_init_fake(uid, NULL);
  } else if ((uintptr_t)*uid == 0x4000) {
    int attr = 1; // recursive
    ret = pthread_mutex_init_fake(uid, &attr);
  }
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;

  *c = PTHREAD_COND_INITIALIZER;

  int ret = pthread_cond_init(c, NULL);
  if (ret < 0) {
    free(c);
    return -1;
  }

  *cnd = c;

  return 0;
}

int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_broadcast(*cnd);
}

int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  };
  return pthread_cond_signal(*cnd);
}

int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_wait(*cnd, *mtx);
}

int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (!*cnd) {
    if (pthread_cond_init_fake(cnd, NULL) < 0)
      return -1;
  }
  return pthread_cond_timedwait(*cnd, *mtx, t);
}

int pthread_once_fake(volatile int *once_control, void (*init_routine) (void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__sync_lock_test_and_set(once_control, 1) == 0)
    (*init_routine)();
  return 0;
}

// pthread_t is an unsigned int, so it should be fine
// TODO: probably shouldn't assume default attributes
int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  return pthread_create(thread, NULL, entry, arg);
}

// GL stuff

void glGetShaderInfoLogHook(GLuint shader, GLsizei maxLength, GLsizei *length, GLchar *infoLog) {
  glGetShaderInfoLog(shader, maxLength, length, infoLog);
  debugPrintf("shader info log:\n%s\n", infoLog);
}

void glCompressedTexImage2DHook(GLenum target, GLint level, GLenum format, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void *data) {
  // don't upload mips
  if (level == 0)
    glCompressedTexImage2D(target, level, format, width, height, border, imageSize, data);
}

void glTexParameteriHook(GLenum target, GLenum param, GLint val) {
  // force trilinear filtering instead of bilinear+nearest mipmap
  if (val == GL_LINEAR_MIPMAP_NEAREST)
    val = GL_LINEAR_MIPMAP_LINEAR;
  glTexParameteri(target, param, val);
}

// CTW extras

// fortify wrappers bionic emits for read()/write()
ssize_t __read_chk(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read(fd, buf, count);
}

ssize_t __write_chk(int fd, const void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return write(fd, buf, count);
}

static int getpid_fake(void) {
  return 1;
}

static int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

static void sincos_fake(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}

// the C++ runtime donor is the APK's libopenal.so; its OpenSLES backend is
// dead code here (the game's AL imports resolve to native openal-soft), but
// its imports still need to resolve to something
static void *SL_IID_fake = NULL;

static unsigned int slCreateEngine_fake(void) {
  return 0x0000000C; // SL_RESULT_FEATURE_UNSUPPORTED
}

// import table

DynLibFunction dynlib_functions[] = {
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit },
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },

  { "stderr", (uintptr_t)&stderr_fake },

  // AAssets are emulated over regular files relative to the game dir
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64_fake },

  // ANativeWindow maps onto the default NWindow
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface_fake },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth_fake },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight_fake },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release_fake },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry_fake },

  // newlib pthread keys are functional, and libc++_shared needs them
  // for emulated thread_local storage
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },

  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },

  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach },
  { "pthread_self", (uintptr_t)&pthread_self },

  { "pthread_setschedparam", (uintptr_t)&ret0 },
  { "pthread_setname_np", (uintptr_t)&ret0 },

  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_setschedparam", (uintptr_t)&ret0 },
  { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_fake },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },

  { "pthread_mutexattr_init", (uintptr_t)&ret0 },
  { "pthread_mutexattr_settype", (uintptr_t)&ret0 },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },

  { "pthread_once", (uintptr_t)&pthread_once_fake },

  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },

  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  { "sched_get_priority_min", (uintptr_t)&retm1 },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },

  { "__android_log_print", (uintptr_t)__android_log_print },
  { "__android_log_write", (uintptr_t)__android_log_write },
  { "__android_log_vprint", (uintptr_t)__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },

  { "__errno", (uintptr_t)&__errno },

  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  // freezes with real __stack_chk_guard
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },

  { "_ctype_", (uintptr_t)&__ctype_ },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },

  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },

  // fortify wrappers
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },

   // TODO: use math neon?
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "log", (uintptr_t)&log },
  { "log10f", (uintptr_t)&log10f },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },

  { "atoi", (uintptr_t)&atoi },
  { "atof", (uintptr_t)&atof },
  { "isspace", (uintptr_t)&isspace },
  { "tolower", (uintptr_t)&tolower },
  { "towlower", (uintptr_t)&towlower },
  { "toupper", (uintptr_t)&toupper },
  { "towupper", (uintptr_t)&towupper },

  { "calloc", (uintptr_t)&calloc },
  { "free", (uintptr_t)&free },
  { "malloc", (uintptr_t)&malloc },
  { "realloc", (uintptr_t)&realloc },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },

  { "clock_gettime", (uintptr_t)&clock_gettime },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "time", (uintptr_t)&time },
  { "asctime", (uintptr_t)&asctime },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "strftime", (uintptr_t)&strftime },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },

  // EGL: the game creates and manages its own context now
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateContext", (uintptr_t)&eglCreateContext },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglDestroySurface", (uintptr_t)&eglDestroySurface },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext },
  { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  // hooked so the movie player can overlay video frames before each swap
  { "eglSwapBuffers", (uintptr_t)&eglSwapBuffersHook },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglTerminate", (uintptr_t)&eglTerminate },
  { "eglBindAPI", (uintptr_t)&eglBindAPI },

  // OpenAL: imported by libGame.so since 2.x; alcOpenDevice/alcCreateContext
  // go through hooks for the 44100hz override
  { "alBufferData", (uintptr_t)&alBufferData },
  { "alDeleteBuffers", (uintptr_t)&alDeleteBuffers },
  { "alDeleteSources", (uintptr_t)&alDeleteSources },
  { "alDistanceModel", (uintptr_t)&alDistanceModel },
  { "alGenBuffers", (uintptr_t)&alGenBuffers },
  { "alGenSources", (uintptr_t)&alGenSources },
  { "alGetEnumValue", (uintptr_t)&alGetEnumValue },
  { "alGetError", (uintptr_t)&alGetError },
  { "alGetSourcef", (uintptr_t)&alGetSourcef },
  { "alGetSourcei", (uintptr_t)&alGetSourcei },
  { "alGetString", (uintptr_t)&alGetString },
  { "alIsExtensionPresent", (uintptr_t)&alIsExtensionPresent },
  { "alListener3f", (uintptr_t)&alListener3f },
  { "alListenerf", (uintptr_t)&alListenerf },
  { "alListenerfv", (uintptr_t)&alListenerfv },
  { "alSource3f", (uintptr_t)&alSource3f },
  { "alSourcePause", (uintptr_t)&alSourcePause },
  { "alSourcePlay", (uintptr_t)&alSourcePlay },
  { "alSourceQueueBuffers", (uintptr_t)&alSourceQueueBuffers },
  { "alSourceStop", (uintptr_t)&alSourceStop },
  { "alSourceUnqueueBuffers", (uintptr_t)&alSourceUnqueueBuffers },
  { "alSourcef", (uintptr_t)&alSourcef },
  { "alSourcei", (uintptr_t)&alSourcei },
  { "alcCloseDevice", (uintptr_t)&alcCloseDevice },
  { "alcCreateContext", (uintptr_t)&alcCreateContextHook },
  { "alcDestroyContext", (uintptr_t)&alcDestroyContext },
  { "alcGetError", (uintptr_t)&alcGetError },
  { "alcGetString", (uintptr_t)&alcGetString },
  { "alcMakeContextCurrent", (uintptr_t)&alcMakeContextCurrent },
  { "alcOpenDevice", (uintptr_t)&alcOpenDeviceHook },

  // mpg123 (music streaming); was libVendor_mpg123.so on Android,
  // provided natively by the switch-mpg123 portlib here
  { "mpg123_delete", (uintptr_t)&mpg123_delete },
  { "mpg123_exit", (uintptr_t)&mpg123_exit },
  { "mpg123_feed", (uintptr_t)&mpg123_feed },
  { "mpg123_feedseek", (uintptr_t)&mpg123_feedseek },
  { "mpg123_format_all", (uintptr_t)&mpg123_format_all },
  { "mpg123_getformat", (uintptr_t)&mpg123_getformat },
  { "mpg123_info", (uintptr_t)&mpg123_info },
  { "mpg123_init", (uintptr_t)&mpg123_init },
  { "mpg123_new", (uintptr_t)&mpg123_new },
  { "mpg123_open_feed", (uintptr_t)&mpg123_open_feed },
  { "mpg123_outblock", (uintptr_t)&mpg123_outblock },
  { "mpg123_read", (uintptr_t)&mpg123_read },

  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },

  { "fopen", (uintptr_t)&fopen_fake },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgetc", (uintptr_t)&fgetc },
  { "fgets", (uintptr_t)&fgets },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof },
  { "fileno", (uintptr_t)&fileno_fake },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "getwc", (uintptr_t)&getwc },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "fputwc", (uintptr_t)&fputwc },

  { "getenv", (uintptr_t)&getenv },

  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
  { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLogHook },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glHint", (uintptr_t)&glHint },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glIsTexture", (uintptr_t)&glIsTexture },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glTexImage2D", (uintptr_t)&glTexImage2D },
  { "glTexParameterf", (uintptr_t)&glTexParameterf },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
  { "glViewport", (uintptr_t)&glViewport },

  // this only uses setjmp in the JPEG loader but not longjmp
  // probably doesn't matter if they're compatible or not
  { "setjmp", (uintptr_t)&setjmp },
  { "longjmp", (uintptr_t)&longjmp },

  { "memcmp", (uintptr_t)&memcmp },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "memchr", (uintptr_t)&memchr },

  { "printf", (uintptr_t)&debugPrintf },

  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },

  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },

  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "swprintf", (uintptr_t)&swprintf },

  { "close", (uintptr_t)&close },
  { "lseek", (uintptr_t)&lseek },
  { "mkdir", (uintptr_t)&mkdir },
  { "open", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "read", (uintptr_t)&read },
  { "write", (uintptr_t)&write },
  { "stat", (uintptr_t)&stat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "unlink", (uintptr_t)&unlink },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "truncate", (uintptr_t)&retm1 },
  { "link", (uintptr_t)&retm1 },
  { "symlink", (uintptr_t)&retm1 },
  { "readlink", (uintptr_t)&retm1 },
  { "chdir", (uintptr_t)&chdir },
  { "getcwd", (uintptr_t)&getcwd },
  { "realpath", (uintptr_t)&realpath_fake },
  { "isatty", (uintptr_t)&isatty },
  { "ioctl", (uintptr_t)&retm1 },
  { "fchmod", (uintptr_t)&ret0 },
  { "fchmodat", (uintptr_t)&ret0 },
  { "utimensat", (uintptr_t)&ret0 },
  { "sendfile", (uintptr_t)&retm1 },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },

  { "opendir", (uintptr_t)&opendir },
  { "fdopendir", (uintptr_t)&ret0 },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "readdir64", (uintptr_t)&readdir_fake },

  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },

  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strcpy", (uintptr_t)&strcpy },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strtod", (uintptr_t)&strtod },
  { "strtok", (uintptr_t)&strtok },
  { "strtol", (uintptr_t)&strtol },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtof", (uintptr_t)&strtof },
  { "strtold", (uintptr_t)&strtold },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull", (uintptr_t)&strtoull },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },

  { "srand", (uintptr_t)&srand },
  { "rand", (uintptr_t)&rand },

  // locale: the _l variants ignore the locale and use the C locale
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  { "wctob", (uintptr_t)&wctob },
  { "wctype", (uintptr_t)&wctype },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "iswctype", (uintptr_t)&iswctype },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcslen", (uintptr_t)&wcslen },
  { "btowc", (uintptr_t)&btowc },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },

  // --- CTW 4.4.243 additions ---

  // libGame.so extras over the Max Payne 2.1.131 import set
  { "__read_chk", (uintptr_t)&__read_chk },
  { "__write_chk", (uintptr_t)&__write_chk },
  { "atan", (uintptr_t)&atan },
  { "expf", (uintptr_t)&expf },
  { "frexp", (uintptr_t)&frexp },
  { "logf", (uintptr_t)&logf },
  { "modf", (uintptr_t)&modf },
  { "gmtime", (uintptr_t)&gmtime },
  { "getpid", (uintptr_t)&getpid_fake },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glVertexAttrib2f", (uintptr_t)&glVertexAttrib2f },
  { "glVertexAttrib3f", (uintptr_t)&glVertexAttrib3f },
  { "glVertexAttrib4f", (uintptr_t)&glVertexAttrib4f },
  { "alBufferi", (uintptr_t)&alBufferi },
  { "alcGetProcAddress", (uintptr_t)&alcGetProcAddress },
  { "alcProcessContext", (uintptr_t)&alcProcessContext },
  { "alcSuspendContext", (uintptr_t)&alcSuspendContext },
  { "mpg123_param", (uintptr_t)&mpg123_param },

  // imports of the C++ runtime donor (the APK's libopenal.so)
  { "__assert2", (uintptr_t)&__assert2 },
  { "__readlink_chk", (uintptr_t)&retm1 },
  { "atan2", (uintptr_t)&atan2 },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "exp2f", (uintptr_t)&exp2f },
  { "hypot", (uintptr_t)&hypot },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "log2f", (uintptr_t)&log2f },
  { "sinhf", (uintptr_t)&sinhf },
  { "sincos", (uintptr_t)&sincos_fake },
  { "clearerr", (uintptr_t)&clearerr },
  { "rewind", (uintptr_t)&rewind },
  { "raise", (uintptr_t)&raise },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "slCreateEngine", (uintptr_t)&slCreateEngine_fake },
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_fake },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_fake },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_fake },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_fake },
  { "SL_IID_RECORD", (uintptr_t)&SL_IID_fake },
  // thread_local init wrapper; safe to no-op (the TLS pointer it would
  // initialize is zero-initialized anyway)
  { "_ZTHN10ALCcontext13sLocalContextE", (uintptr_t)&ret0 },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void update_imports(void) {
  // only use the hooks if the relevant config options are enabled to avoid possible overhead
  if (config.disable_mipmaps)
    so_find_import(dynlib_functions, dynlib_numfunctions, "glCompressedTexImage2D")->func = (uintptr_t)glCompressedTexImage2DHook;
  if (config.trilinear_filter)
    so_find_import(dynlib_functions, dynlib_numfunctions, "glTexParameteri")->func = (uintptr_t)glTexParameteriHook;
}
