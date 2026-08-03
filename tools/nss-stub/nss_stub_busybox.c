/*
 * Same fix as tools/nss-stub/nss_stub.c (glibc >=2.34 static NSS/dlopen
 * init assertion crash on this toolchain, see tools/nss-stub/README.md),
 * but with a different symbol set than nano/htop/tmux/gdbserver needed.
 * getpwnam/getpwuid/getgrnam/getgrgid/getpwent/setpwent/endpwent/
 * getgrent/setgrent/endgrent are NOT wrapped here -- busybox's own
 * libpwdgrp/pwd_grp.c (CONFIG_USE_BB_PWD_GRP=y) already provides real,
 * self-contained, non-NSS definitions of every one of those names, so
 * they never reference glibc's real (dlopen-NSS-backed) versions at
 * all -- wrapping them anyway would collide with busybox's own
 * definitions at link time ("multiple definition of __wrap_getpwent").
 *
 * What busybox *does* still reference for real, unwrapped glibc NSS:
 * host/service resolution (getaddrinfo/gethostbyname/gethostbyaddr/
 * getservbyname/getservbyport, used by ipcalc/netstat/inetd/ping-style
 * applets) -- confirmed via this build's own "Using 'X' in statically
 * linked applications requires..." linker warnings -- and dlopen
 * (defensive, in case any applet path pulls it in). Both go through the
 * same broken static-dlopen-NSS init machinery, so both need wrapping.
 */
#include <netdb.h>
#include <sys/socket.h>
#include <stddef.h>

int __wrap_getaddrinfo(const char *node, const char *service,
			const void *hints, void *res) {
	(void)node; (void)service; (void)hints; (void)res;
	return EAI_NONAME;
}
void __wrap_freeaddrinfo(void *res) { (void)res; }
const char *__wrap_gai_strerror(int errcode) {
	(void)errcode;
	return "name resolution disabled in this static build";
}
struct hostent *__wrap_gethostbyname(const char *name) { (void)name; return NULL; }
struct hostent *__wrap_gethostbyname2(const char *name, int af) {
	(void)name; (void)af;
	return NULL;
}
struct hostent *__wrap_gethostbyaddr(const void *addr, socklen_t len, int type) {
	(void)addr; (void)len; (void)type;
	return NULL;
}
struct servent *__wrap_getservbyname(const char *name, const char *proto) {
	(void)name; (void)proto;
	return NULL;
}
struct servent *__wrap_getservbyport(int port, const char *proto) {
	(void)port; (void)proto;
	return NULL;
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
