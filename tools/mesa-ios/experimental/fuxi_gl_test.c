/*
 * fuxi_gl_test — the smallest check that the Mesa stack on fuxi can draw at all.
 *
 * It makes an OpenGL ES context on libOSMesa.8.dylib, clears a known colour,
 * reads the pixel back through the driver, and then says so in one of two ways
 * that need no shell on the device:
 *
 *   success -> the machine powers itself off (reboot(2) with RB_POWEROFF)
 *   failure -> nothing happens; the phone keeps running
 *
 * So "the phone shut down by itself a minute after booting" means the GL stack
 * works, and "it is still on" means it does not. The log at /redwood/gl-test.log
 * carries the details for the next step (it survives on the root volume).
 *
 * Built by tools/mesa-ios/build-gl-test.sh (CI: the Mesa iOS workflow) against
 * the same libOSMesa.8.dylib that libredwood-gl.dylib links.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>

/* the OSMesa surface interface, as gl_osmesa.c declares it */
#define OSMESA_RGBA                     0x1908
#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
#define GL_UNSIGNED_BYTE                0x1401

typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern void (*OSMesaGetProcAddress(const char *name))(void);

extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float r, float g, float b, float a);
extern void glClear(unsigned mask);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void *pixels);

#define GL_VENDOR    0x1f00
#define GL_RENDERER  0x1f01
#define GL_VERSION   0x1f02
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA      0x1908

#define WIDTH  64
#define HEIGHT 64
#define LOG    "/redwood/gl-test.log"

static FILE *logf;

static void say(const char *fmt, ...)
{
	va_list ap;
	if (logf != NULL) {
		va_start(ap, fmt);
		vfprintf(logf, fmt, ap);
		va_end(ap);
		fflush(logf);
	}
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int
main(int argc, char **argv)
{
	unsigned char window[WIDTH * HEIGHT * 4];
	unsigned char pixel[4] = { 0, 0, 0, 0 };
	OSMesaContext context;
	const int attributes[] = {
		OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_STENCIL_BITS, 8,
		OSMESA_PROFILE, OSMESA_ES_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 3, OSMESA_CONTEXT_MINOR_VERSION, 0,
		0
	};
	int ok = 0;

	(void)argc;
	(void)argv;
	logf = fopen(LOG, "a");
	say("fuxi_gl_test: start (%s)\n", OSMesaGetProcAddress != NULL ? "symbols resolved" : "no symbols");

	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL) {
		say("fuxi_gl_test: OSMesaCreateContextAttribs(ES3) failed, trying ES2\n");
		attributes[9] = 2;
		context = OSMesaCreateContextAttribs(attributes, NULL);
	}
	if (context == NULL) {
		say("fuxi_gl_test: no context at all -> FAIL\n");
		goto out;
	}
	if (!OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, WIDTH, HEIGHT)) {
		say("fuxi_gl_test: OSMesaMakeCurrent failed -> FAIL\n");
		goto out;
	}

	say("fuxi_gl_test: vendor=%s\n", glGetString(GL_VENDOR) ? (const char *)glGetString(GL_VENDOR) : "?");
	say("fuxi_gl_test: renderer=%s\n", glGetString(GL_RENDERER) ? (const char *)glGetString(GL_RENDERER) : "?");
	say("fuxi_gl_test: version=%s\n", glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "?");

	glViewport(0, 0, WIDTH, HEIGHT);
	glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	say("fuxi_gl_test: pixel = %u %u %u %u, error = 0x%x\n",
	    pixel[0], pixel[1], pixel[2], pixel[3], glGetError());

	/* 0.2/0.4/0.8 of 255 = 51/102/204; allow rounding */
	if (pixel[0] > 40 && pixel[0] < 62 && pixel[1] > 90 && pixel[1] < 114 &&
	    pixel[2] > 190 && pixel[2] < 218) {
		ok = 1;
	}

out:
	if (ok) {
		say("fuxi_gl_test: PASS -> powering off in 5 s (this is the signal)\n");
		if (logf) {
			fclose(logf);
			logf = NULL;
		}
		sleep(5);
		sync();
		reboot(RB_POWEROFF);
		/* if reboot is not permitted here, fall through to a visible exit */
		say("fuxi_gl_test: poweroff was refused, staying up\n");
		return 0;
	}
	say("fuxi_gl_test: FAIL -> staying up\n");
	if (logf) {
		fclose(logf);
	}
	return 1;
}
