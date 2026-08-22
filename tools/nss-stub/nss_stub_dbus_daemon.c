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
 * does), so plain no-op stubs for dlopen/getaddrinfo, same as nss_stub.c.
 *
 * 2026-08-20 REVISED: getpwnam_r/getpwuid_r/getgrnam_r/getgrgid_r are
 * NOT plain no-ops here, unlike every other provider in this dir --
 * real hardware showed dbus-daemon itself failing to start entirely:
 *   Unknown username "root" in message bus configuration file
 *   Failed to start message bus: Could not get UID and GID for username "root"
 * Root cause: unlike bluetoothd (which runs AS uid 0 and never needs to
 * resolve a username by NAME), dbus-daemon's own config parser
 * genuinely calls getpwnam_r("root", ...) to resolve this device's
 * real system-diagnostic.conf's <user>root</user> policy directive at
 * startup -- a real, load-bearing lookup this process cannot function
 * without, not a dead code path safe to stub out unconditionally.
 * "root" (uid/gid 0) is hardcoded below rather than doing a real
 * /etc/passwd parse -- this device's own /etc/passwd has only that one
 * entry (see docs/1.4_WIRELESS_AND_INIT.md and this project's own
 * established convention of runtime-root-only elsewhere, e.g.
 * MsnCoreApp's own no-multi-user assumptions), so a real parser would
 * be strictly more code for a result this static build already knows.
 * Every other username/uid still returns "not found", same as before.
 */
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <netdb.h>

struct passwd *__wrap_getpwnam(const char *name) {
	(void)name;
	return NULL;  /* no callers needing the non-reentrant form on this build */
}
struct passwd *__wrap_getpwuid(uid_t uid) {
	(void)uid;
	return NULL;  /* no callers needing the non-reentrant form on this build */
}
int __wrap_getpwnam_r(const char *name, struct passwd *pwd, char *buf,
		       size_t buflen, struct passwd **result) {
	if (name && strcmp(name, "root") == 0 && buflen >= 32) {
		pwd->pw_name = buf;
		strcpy(buf, "root");
		pwd->pw_passwd = buf + 5;
		buf[5] = '\0';
		pwd->pw_uid = 0;
		pwd->pw_gid = 0;
		pwd->pw_gecos = buf + 5;
		pwd->pw_dir = (char *)"/";
		pwd->pw_shell = (char *)"/bin/sh";
		*result = pwd;
		return 0;
	}
	(void)buf; (void)buflen;
	*result = NULL;
	return 0;
}
int __wrap_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
		       size_t buflen, struct passwd **result) {
	if (uid == 0 && buflen >= 32) {
		pwd->pw_name = buf;
		strcpy(buf, "root");
		pwd->pw_passwd = buf + 5;
		buf[5] = '\0';
		pwd->pw_uid = 0;
		pwd->pw_gid = 0;
		pwd->pw_gecos = buf + 5;
		pwd->pw_dir = (char *)"/";
		pwd->pw_shell = (char *)"/bin/sh";
		*result = pwd;
		return 0;
	}
	*result = NULL;
	return 0;
}
struct passwd *__wrap_getpwent(void) { return NULL; }
void __wrap_setpwent(void) {}
void __wrap_endpwent(void) {}
struct group *__wrap_getgrgid(gid_t gid) { (void)gid; return NULL; }
int __wrap_getgrgid_r(gid_t gid, struct group *grp, char *buf,
		       size_t buflen, struct group **result) {
	if (gid == 0 && buflen >= 32) {
		static char *no_members[] = { NULL };
		grp->gr_name = buf;
		strcpy(buf, "root");
		grp->gr_passwd = buf + 5;
		buf[5] = '\0';
		grp->gr_gid = 0;
		grp->gr_mem = no_members;
		*result = grp;
		return 0;
	}
	*result = NULL;
	return 0;
}
int __wrap_getgrnam_r(const char *name, struct group *grp, char *buf,
		       size_t buflen, struct group **result) {
	if (name && strcmp(name, "root") == 0 && buflen >= 32) {
		static char *no_members[] = { NULL };
		grp->gr_name = buf;
		strcpy(buf, "root");
		grp->gr_passwd = buf + 5;
		buf[5] = '\0';
		grp->gr_gid = 0;
		grp->gr_mem = no_members;
		*result = grp;
		return 0;
	}
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
