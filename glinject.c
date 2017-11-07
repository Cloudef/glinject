/* gcc -std=c99 -fPIC -shared -Wl,-soname,glinject.so glinject.c -o glinject.so
 * gcc -m32 -std=c99 -fPIC -shared -Wl,-soname,glinject.so glinject.c -o glinject.so (for 32bit)
 *
 * Env vars:
 * SHADER_FILTER: Filter by executing program with shader source as content of stdin
 *
 * Inject custom data into OpenGL program.
 * Usage: LD_PRELOAD="/path/to/glinject.so" ./program
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/wait.h>
#include <err.h>

#include <GL/glx.h>
#include <EGL/egl.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define WARN(x, ...) do { warn("glinject: "x, ##__VA_ARGS__); } while (0)
#define WARNX(x, ...) do { warnx("glinject: "x, ##__VA_ARGS__); } while (0)
#define ERRX(x, y, ...) do { errx(x, "glinject: "y, ##__VA_ARGS__); } while (0)
#define ERR(x, y, ...) do { err(x, "glinject: "y, ##__VA_ARGS__); } while (0)
#define WARN_ONCE(x, ...) do { static bool o = false; if (!o) { WARNX(x, ##__VA_ARGS__); o = true; } } while (0)

static void* (*_dlsym)(void*, const char*);
static __eglMustCastToProperFunctionPointerType (*_eglGetProcAddress)(const char*);
static __GLXextFuncPtr (*_glXGetProcAddress)(const GLubyte*);
static __GLXextFuncPtr (*_glXGetProcAddressARB)(const GLubyte*);
static void (*_glShaderSource)(GLuint, GLsizei, const GLchar *const*, const GLint *) = NULL;
static void* store_real_symbol_and_return_fake_symbol(const char*, void*);
static void hook_function(void**, const char*, const bool, const char*[]);
static void hook_dlsym(void**, const char*);

#define HOOK(x) hook_function((void**)&_##x, #x, false, NULL)
#define HOOK_FROM(x, ...) hook_function((void**)&_##x, #x, false, (const char*[]){ __VA_ARGS__, NULL })

// Use HOOK_FROM with this list for any GL/GLX stuff
#define GL_LIBS "libGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLX.so"

static void
close_fd(int *fd)
{
   assert(fd);
   if (*fd >= 0)
      close(*fd);
}

struct proc {
   pid_t pid;
   int fds[2];
};

bool
proc_open(const char *bin, struct proc *out_proc)
{
   assert(bin && out_proc);
   *out_proc = (struct proc){0};

   int pipes[4];
   pipe(&pipes[0]); /* parent */
   pipe(&pipes[2]); /* child */

   if ((out_proc->pid = fork()) > 0) {
      out_proc->fds[0] = pipes[3];
      out_proc->fds[1] = pipes[0];
      close(pipes[1]);
      close(pipes[2]);
      return true;
   } else {
      close(pipes[0]);
      close(pipes[3]);
      dup2(pipes[2], 0);
      dup2(pipes[1], 1);
      execlp(bin, bin, NULL);
      _exit(0);
   }

   return false;
}

void
proc_close(struct proc *proc)
{
   assert(proc);
   waitpid(proc->pid, NULL, 0);
   close_fd(&proc->fds[0]);
   close_fd(&proc->fds[1]);
   *proc = (struct proc){0};
}

__eglMustCastToProperFunctionPointerType
eglGetProcAddress(const char *procname)
{
   HOOK_FROM(eglGetProcAddress, "libEGL.so");
   return (_eglGetProcAddress ? store_real_symbol_and_return_fake_symbol(procname, _eglGetProcAddress(procname)) : NULL);
}

__GLXextFuncPtr
glXGetProcAddressARB(const GLubyte *procname)
{
   HOOK_FROM(glXGetProcAddressARB, GL_LIBS);
   return (_glXGetProcAddressARB ? store_real_symbol_and_return_fake_symbol((const char*)procname, _glXGetProcAddressARB(procname)) : NULL);
}

__GLXextFuncPtr
glXGetProcAddress(const GLubyte *procname)
{
   HOOK_FROM(glXGetProcAddress, GL_LIBS);
   return (_glXGetProcAddress ? store_real_symbol_and_return_fake_symbol((const char*)procname, _glXGetProcAddress(procname)) : NULL);
}

void
glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length)
{
   HOOK_FROM(glShaderSource, GL_LIBS);

   const char *filter;
   if ((filter = getenv("SHADER_FILTER"))) {
      struct proc proc;
      if (!proc_open(filter, &proc))
         ERRX(EXIT_FAILURE, "Could not execute filter: %s", filter);

      for (GLsizei i = 0; i < count; ++i) {
         if (length) {
            write(proc.fds[0], string[i], length[i]);
         } else {
            write(proc.fds[0], string[i], strlen(string[i]));
         }
      }

      static const size_t SHADER_MAX_SIZE = 4096 * 1024;
      static __thread char *src;
      if (!src && !(src = malloc(SHADER_MAX_SIZE)))
         ERRX(EXIT_FAILURE, "no memory");

      close_fd(&proc.fds[0]);
      ssize_t ret = read(proc.fds[1], src, SHADER_MAX_SIZE);
      ret = (ret < 0 ? 0 : ret);
      proc_close(&proc);

      _glShaderSource(shader, 1, (const GLchar*[]){src}, (const GLint[]){ret});
   } else {
      _glShaderSource(shader, count, string, length);
   }
}

static void*
store_real_symbol_and_return_fake_symbol(const char *symbol, void *ret)
{
   if (!ret || !symbol)
      return ret;

   if (0) {}
#define SET_IF_NOT_HOOKED(x, y) do { if (!_##x) { _##x = y; WARNX("SET %s to %p", #x, y); } } while (0)
#define FAKE_SYMBOL(x) else if (!strcmp(symbol, #x)) { SET_IF_NOT_HOOKED(x, ret); return x; }
   FAKE_SYMBOL(eglGetProcAddress)
   FAKE_SYMBOL(glXGetProcAddressARB)
   FAKE_SYMBOL(glXGetProcAddress)
   FAKE_SYMBOL(glShaderSource)
#undef FAKE_SYMBOL
#undef SET_IF_NOT_HOOKED

   return ret;
}

#define HOOK_DLSYM(x) hook_dlsym((void**)&_##x, #x)

static void*
get_symbol(void *src, const char *name, const bool versioned)
{
   if (!src)
      return NULL;

   if (versioned) {
      void *ptr = NULL;
      const char *versions[] = { "GLIBC_2.0", "GLIBC_2.2.5" };
      for (size_t i = 0; !ptr && i < ARRAY_SIZE(versions); ++i)
         ptr = dlvsym(src, name, versions[i]);
      return ptr;
   }

   HOOK_DLSYM(dlsym);
   return _dlsym(src, name);
}

static void
hook_function(void **ptr, const char *name, const bool versioned, const char *srcs[])
{
   if (*ptr)
      return;

   *ptr = get_symbol(RTLD_NEXT, name, versioned);

   for (size_t i = 0; !*ptr && srcs && srcs[i]; ++i) {
      // If we know where the symbol comes from, but program e.g. used dlopen with RTLD_LOCAL
      // Should be only needed with GL/GLES/EGL stuff as we don't link to those for reason.
      void *so = dlopen(srcs[i], RTLD_LAZY | RTLD_NOLOAD);
      WARNX("Trying dlopen: %s (%p) (RTLD_LAZY | RTLD_NOLOAD)", srcs[i], so);
      *ptr = get_symbol(so, name, versioned);
   }

   if (!*ptr)
      ERRX(EXIT_FAILURE, "HOOK FAIL %s", name);

   WARNX("HOOK %s", name);
}

static void
hook_dlsym(void **ptr, const char *name)
{
   if (*ptr)
      return;

   hook_function(ptr, name, true, NULL);

   void *next;
   if ((next = _dlsym(RTLD_NEXT, name))) {
      WARNX("chaining %s: %p -> %p", name, ptr, next);
      *ptr = next;
   }
}

void*
dlsym(void *handle, const char *symbol)
{
   HOOK_DLSYM(dlsym);

   if (!strcmp(symbol, "dlsym"))
      return dlsym;

   return store_real_symbol_and_return_fake_symbol(symbol, _dlsym(handle, symbol));
}
