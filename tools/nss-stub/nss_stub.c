/*
 * Stub out NSS/dlopen-touching libc calls entirely, rather than just not
 * calling them. glibc >=2.34 merged NSS's dlopen-based service-module
 * loading into libc.a itself -- even a fully static binary that merely
 * *references* getpwnam/getpwuid/dlopen (whether or not the call path is
 * ever actually exercised at runtime) pulls in that static-dlopen-NSS
 * init machinery, which asserts and crashes at process startup on this
 * toolchain/kernel combination:
 *   dl-call-libc-early-init.c:37: _dl_call_libc_early_init:
 *   Assertion `sym != NULL' failed.
 * None of these lookups matter on this target anyway -- no /etc/nsswitch.conf,
 * a flat /etc/passwd only ever consulted for uid 0, and no systemd to dlopen.
 * Wrapping (-Wl,--wrap=...) means the real glibc symbols are never linked
 * in at all, so the broken init path is never reached.
 */
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <stddef.h>

struct passwd *__wrap_getpwnam(const char *name) { (void)name; return NULL; }
struct passwd *__wrap_getpwuid(uid_t uid) { (void)uid; return NULL; }
int __wrap_getpwnam_r(const char *name, struct passwd *pwd, char *buf,
		       size_t buflen, struct passwd **result) {
	(void)name; (void)pwd; (void)buf; (void)buflen;
	*result = NULL;
	return 0;
}
int __wrap_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
		       size_t buflen, struct passwd **result) {
	(void)uid; (void)pwd; (void)buf; (void)buflen;
	*result = NULL;
	return 0;
}
struct passwd *__wrap_getpwent(void) { return NULL; }
void __wrap_setpwent(void) {}
void __wrap_endpwent(void) {}
struct group *__wrap_getgrgid(gid_t gid) { (void)gid; return NULL; }
int __wrap_getgrgid_r(gid_t gid, struct group *grp, char *buf,
		       size_t buflen, struct group **result) {
	(void)gid; (void)grp; (void)buf; (void)buflen;
	*result = NULL;
	return 0;
}
void *__wrap_dlopen(const char *filename, int flags) {
	(void)filename; (void)flags;
	return NULL;
}
char *__wrap_dlerror(void) { return (char *)"dlopen disabled in this static build"; }
void *__wrap_dlsym(void *handle, const char *symbol) {
	(void)handle; (void)symbol;
	return NULL;
}
int __wrap_dlclose(void *handle) { (void)handle; return 0; }
