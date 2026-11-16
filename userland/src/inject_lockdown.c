/*
 * libredwood.lockdown.dylib: allow loopback usbmux_relay connections and pairing in lockdownd.
 *
 * 1. lockdownd's socket_accept_callback checks getsockopt(fd, IPPROTO_TCP, 0x204, &pid, &len)
 *    and rejects loopback peers whose pid != 0 ("non kernel peer, pid %d!").
 *    Interpose getsockopt for lockdownd on that option and report pid 0 (kernel peer).
 * 2. When CFUserNotificationCreate fails (no SpringBoard trust dialog server), return a
 *    dummy CF object and answer CFUserNotificationReceiveResponse with button 1 (Trust).
 * 3. Interpose RemotePairingGetUSBConnectedHostTrustState to return 5 (trusted/no remote
 *    pairing daemon needed) when com.apple.remotepairingdevice.pairingmanagement is absent.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define TCP_PEER_PID_OPT 0x204

extern CFUserNotificationRef real_CFUserNotificationCreate(CFAllocatorRef allocator,
    CFTimeInterval timeout, CFOptionFlags flags, SInt32 *error, CFDictionaryRef dictionary)
    __asm__("_CFUserNotificationCreate");
extern SInt32 real_CFUserNotificationReceiveResponse(CFUserNotificationRef userNotification,
    CFTimeInterval timeout, CFOptionFlags *responseFlags)
    __asm__("_CFUserNotificationReceiveResponse");
extern SInt32 real_CFUserNotificationCancel(CFUserNotificationRef userNotification)
    __asm__("_CFUserNotificationCancel");
extern long real_RemotePairingGetUSBConnectedHostTrustState(void **outErr)
    __asm__("_RemotePairingGetUSBConnectedHostTrustState");

static bool ours;
static CFTypeRef dummy_notif;

static int
watched_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
	if (ours && level == IPPROTO_TCP && optname == TCP_PEER_PID_OPT &&
	    optval != NULL && optlen != NULL && *optlen >= (socklen_t)sizeof(int)) {
		*(int *)optval = 0;
		*optlen = (socklen_t)sizeof(int);
		return 0;
	}
	return getsockopt(s, level, optname, optval, optlen);
}

static CFUserNotificationRef
watched_CFUserNotificationCreate(CFAllocatorRef allocator, CFTimeInterval timeout,
    CFOptionFlags flags, SInt32 *error, CFDictionaryRef dictionary)
{
	CFUserNotificationRef r = real_CFUserNotificationCreate(allocator, timeout, flags, error, dictionary);
	if (r != NULL || !ours || dummy_notif == NULL) {
		return r;
	}
	if (error != NULL) *error = 0;
	return (CFUserNotificationRef)CFRetain(dummy_notif);
}

static SInt32
watched_CFUserNotificationReceiveResponse(CFUserNotificationRef userNotification,
    CFTimeInterval timeout, CFOptionFlags *responseFlags)
{
	if (ours && dummy_notif != NULL && userNotification == (CFUserNotificationRef)dummy_notif) {
		if (responseFlags != NULL) *responseFlags = 1; /* kCFUserNotificationAlternateResponse (Trust) */
		return 0;
	}
	return real_CFUserNotificationReceiveResponse(userNotification, timeout, responseFlags);
}

static SInt32
watched_CFUserNotificationCancel(CFUserNotificationRef userNotification)
{
	if (ours && dummy_notif != NULL && userNotification == (CFUserNotificationRef)dummy_notif) {
		return 0;
	}
	return real_CFUserNotificationCancel(userNotification);
}

static long
watched_RemotePairingGetUSBConnectedHostTrustState(void **outErr)
{
	if (ours) {
		if (outErr != NULL) *outErr = NULL;
		return 5;
	}
	return real_RemotePairingGetUSBConnectedHostTrustState(outErr);
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} interposed[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_getsockopt, (const void *)getsockopt },
	{ (const void *)watched_CFUserNotificationCreate, (const void *)real_CFUserNotificationCreate },
	{ (const void *)watched_CFUserNotificationReceiveResponse, (const void *)real_CFUserNotificationReceiveResponse },
	{ (const void *)watched_CFUserNotificationCancel, (const void *)real_CFUserNotificationCancel },
	{ (const void *)watched_RemotePairingGetUSBConnectedHostTrustState, (const void *)real_RemotePairingGetUSBConnectedHostTrustState },
};

__attribute__((constructor))
static void
redwood_lockdown_init(void)
{
	const char *name = getprogname();
	ours = (name != NULL && strcmp(name, "lockdownd") == 0);
	if (ours) {
		int one = 1;
		dummy_notif = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &one);
	}
}
