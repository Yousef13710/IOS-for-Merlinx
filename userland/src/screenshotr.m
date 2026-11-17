/*
 * screenshotr: on-demand com.apple.mobile.screenshotr service for lockdownd / idevicescreenshot.
 * Spawned by lockdownd via /Library/Lockdown/ServiceAgents/com.apple.mobile.screenshotr.plist.
 * Reads the active display framebuffer via sysctl kern.redwood.fbgrab and sends a PNG over
 * Apple's DeviceLink (binary plist + 4-byte BE length) protocol. Zero idle overhead.
 */
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

extern int secure_lockdown_checkin(void **conn, CFDictionaryRef unused1, CFDictionaryRef unused2);
extern ssize_t lockdown_recv(void *conn, void *buffer, size_t length);
extern ssize_t lockdown_send(void *conn, const void *buffer, size_t length);
extern void lockdown_disconnect(void *conn);

static int
send_all(void *conn, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t sent = 0;

	while (sent < len) {
		ssize_t n = lockdown_send(conn, p + sent, len - sent);
		if (n <= 0) {
			return -1;
		}
		sent += (size_t)n;
	}
	return 0;
}

static int
recv_all(void *conn, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t got = 0;

	while (got < len) {
		ssize_t n = lockdown_recv(conn, p + got, len - got);
		if (n <= 0) {
			return -1;
		}
		got += (size_t)n;
	}
	return 0;
}

static int
dl_send_plist(void *conn, id obj)
{
	NSError *err = nil;
	NSData *plist = [NSPropertyListSerialization dataWithPropertyList:obj
	    format:NSPropertyListBinaryFormat_v1_0 options:0 error:&err];
	if (plist == nil || plist.length > 64 * 1024 * 1024) {
		return -1;
	}
	uint32_t be_len = htonl((uint32_t)plist.length);
	if (send_all(conn, &be_len, sizeof(be_len)) != 0) {
		return -1;
	}
	return send_all(conn, plist.bytes, plist.length);
}

static id
dl_recv_plist(void *conn)
{
	uint32_t be_len = 0;

	if (recv_all(conn, &be_len, sizeof(be_len)) != 0) {
		return nil;
	}
	uint32_t len = ntohl(be_len);
	if (len == 0 || len > 16 * 1024 * 1024) {
		return nil;
	}
	NSMutableData *buf = [NSMutableData dataWithLength:len];
	if (buf == nil || recv_all(conn, buf.mutableBytes, len) != 0) {
		return nil;
	}
	NSError *err = nil;
	return [NSPropertyListSerialization propertyListWithData:buf
	    options:NSPropertyListImmutable format:NULL error:&err];
}

static NSData *
capture_png(void)
{
	size_t size = 16 + 3000u * 3200u * 4u;
	uint8_t *buffer = malloc(size);

	if (buffer == NULL) {
		return nil;
	}
	if (sysctlbyname("kern.redwood.fbgrab", buffer, &size, NULL, 0) != 0 || size < 16) {
		free(buffer);
		return nil;
	}
	uint32_t width = ((uint32_t *)buffer)[0];
	uint32_t height = ((uint32_t *)buffer)[1];
	uint32_t stride = ((uint32_t *)buffer)[2];
	if (width == 0 || height == 0 || stride < width * 4 || size < 16 + (size_t)stride * height) {
		free(buffer);
		return nil;
	}

	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	CGContextRef ctx = CGBitmapContextCreate(buffer + 16, width, height, 8, stride, space,
	    kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
	CGImageRef image = ctx != NULL ? CGBitmapContextCreateImage(ctx) : NULL;
	NSMutableData *png = nil;
	if (image != NULL) {
		png = [NSMutableData data];
		CGImageDestinationRef dst = CGImageDestinationCreateWithData(
		    (__bridge CFMutableDataRef)png, CFSTR("public.png"), 1, NULL);
		if (dst != NULL) {
			CGImageDestinationAddImage(dst, image, NULL);
			if (!CGImageDestinationFinalize(dst)) {
				png = nil;
			}
			CFRelease(dst);
		} else {
			png = nil;
		}
		CGImageRelease(image);
	}
	if (ctx != NULL) {
		CGContextRelease(ctx);
	}
	if (space != NULL) {
		CGColorSpaceRelease(space);
	}
	free(buffer);
	return png;
}

int
main(void)
{
	@autoreleasepool {
		void *conn = NULL;
		if (secure_lockdown_checkin(&conn, NULL, NULL) != 0 || conn == NULL) {
			return 1;
		}
		if (dl_send_plist(conn, @[ @"DLMessageVersionExchange", @400, @0 ]) != 0) {
			lockdown_disconnect(conn);
			return 1;
		}
		id ver = dl_recv_plist(conn);
		if (![ver isKindOfClass:[NSArray class]] || [ver count] < 2 ||
		    ![ver[0] isEqual:@"DLMessageVersionExchange"] || ![ver[1] isEqual:@"DLVersionsOk"]) {
			lockdown_disconnect(conn);
			return 1;
		}
		if (dl_send_plist(conn, @[ @"DLMessageDeviceReady" ]) != 0) {
			lockdown_disconnect(conn);
			return 1;
		}

		for (;;) {
			id msg = dl_recv_plist(conn);
			if (![msg isKindOfClass:[NSArray class]] || [msg count] == 0) {
				break;
			}
			NSString *kind = msg[0];
			if ([kind isEqual:@"DLMessageDisconnect"]) {
				break;
			}
			if ([kind isEqual:@"DLMessageProcessMessage"] && [msg count] >= 2 &&
			    [msg[1] isKindOfClass:[NSDictionary class]]) {
				NSDictionary *dict = msg[1];
				if ([dict[@"MessageType"] isEqual:@"ScreenShotRequest"]) {
					NSData *png = capture_png();
					NSDictionary *reply = png != nil
					    ? @{ @"MessageType" : @"ScreenShotReply", @"ScreenShotData" : png }
					    : @{ @"MessageType" : @"ScreenShotReply", @"Error" : @"Framebuffer unavailable" };
					if (dl_send_plist(conn, @[ @"DLMessageProcessMessage", reply ]) != 0) {
						break;
					}
				}
			}
		}
		lockdown_disconnect(conn);
	}
	return 0;
}
