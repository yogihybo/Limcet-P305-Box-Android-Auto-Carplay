/*
 * Same static-NSS-init crash class as nss_stub.c/nss_stub_busybox.c
 * (see nss_stub.c's own top comment for the full mechanism) -- a
 * dedicated provider for dbus-daemon specifically, cross-compiled from
 * the same vendored dbus-1.14.10 source as bluetoothd's own libdbus-1.a
 * (see tools/bluetoothd-test/README.md). dbus-daemon needs the UNION of
 * symbols nss_stub.c (getgrgid_r) and nss_stub_busybox.c/hantro_dlopen.c
 * (getgrnam_r) separately provide -- its own bus/dbus-sysdeps-util-unix.c
 * (fill_group_info(), daemon-only code not present in the client
 * library) references BOTH, which neither existing single-purpose stub
 * file covers alone. No real dlopen needed here (dbus-daemon doesn't
 * load service modules the way androidauto-sidecar's Hantro decoder
 * does), so plain no-op stubs throughout, same as nss_stub.c.
 */
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <stddef.h>
#include <netdb.h>

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
int __wrap_getgrnam_r(const char *name, struct group *grp, char *buf,
		       size_t buflen, struct group **result) {
	(void)name; (void)grp; (void)buf; (void)buflen;
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
int __wrap_getgrouplist(const char *user, gid_t group, gid_t *groups,
			 int *ngroups) {
	(void)user; (void)group;
	if (groups && ngroups && *ngroups > 0)
		groups[0] = group;
	if (ngroups)
		*ngroups = 1;
	return 1;
}
int __wrap_getaddrinfo(const char *node, const char *service,
			const struct addrinfo *hints, struct addrinfo **res) {
	(void)node; (void)service; (void)hints; (void)res;
	return EAI_NONAME;
}
