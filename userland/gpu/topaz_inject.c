/*
 * libtopaz-inject.dylib (private build, local only): inserted into backboardd
 * (DYLD_INSERT_LIBRARIES in its launchd job). When the kernel has published the
 * GPU service (boot-arg gpu=1, TopazAdreno.cpp), it loads the OpenGL ES library
 * that stands behind EAGLContext, so CoreAnimation's window server draws with
 * the Adreno instead of its software renderer. gl=0 in the boot-args: never.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <syslog.h>
#include <signal.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/ucontext.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <objc/objc-exception.h>
#include <mach/mach.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <mach-o/dyld.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define TOPAZ_GL "/topaz/lib/libtopaz-gl.dylib"

/*
 * CoreAnimation's OpenGL ES renderer hands a "function"'s numbers (a colour matrix, ...) to
 * GLContext::function_uniform(); for the functions it has no shader for they go nowhere, and our
 * library draws those with programs of its own (eagl.c) once told them. Without that the lock
 * screen's vibrantColorMatrix layer is a red veil. (Scheme from darwin-redwood inject.c; addresses
 * are this iOS 16.7.16 cache's.)
 */
#define CACHE_BASE              0x180000000UL
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* vtable for CA::OGL::GLESContext */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))
#define FN_FUNCTION_SUPPORTED   0x1884b2164UL   /* CA::OGL::GLContext::function_supported() const */
#define FN_FUNCTION_UNIFORM     0x1884b21acUL   /* CA::OGL::GLContext::function_uniform(unsigned, unsigned long, unsigned long, float const*) */

static uintptr_t slide;

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	void *original;

	if (*entry != expected + slide) {
		fprintf(stderr, "topaz-gl: %s: slot holds %#lx, expected %#lx: left alone\n", what, *entry - slide, expected);
		return NULL;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		fprintf(stderr, "topaz-gl: %s: table not writable\n", what);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

static int (*orig_function_supported)(void *context);
static void (*gl_function_refused)(unsigned function);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static unsigned
context_function(void *context)
{
	return *(*(uint8_t **)((char *)context + 0x10) + 0x10);
}

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	unsigned function = context_function(context);

	if (!supported) {
		if (gl_function_refused != NULL) {
			gl_function_refused(function);
		}
		if (!said[function & 255]) {
			said[function & 255] = 1;
			fprintf(stderr, "topaz-gl: CoreAnimation's GL renderer has no function %u\n", function);
		}
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	gl_function_values(context, context_function(context), index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

static void
gl_hooks(void *lib)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (cache == NULL || lib == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	gl_function_refused = (void (*)(unsigned))dlsym(lib, "redwood_gl_function_refused");
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(lib,
	    "redwood_gl_function_values");
	orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), FN_FUNCTION_SUPPORTED,
	    my_function_supported);
	if (gl_function_values != NULL) {
		orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), FN_FUNCTION_UNIFORM,
		    my_function_uniform);
	}
	fprintf(stderr, "topaz-gl: GLESContext hooks: function_supported %s, function_uniform %s\n",
	    orig_function_supported ? "on" : "OFF", orig_function_uniform ? "on" : "OFF");
}

/* Where a crash happened, printed to stderr (the console): image + offset + symbol. */
static void
topaz_where(const char *what, void *addr)
{
	Dl_info info;

	if (dladdr(addr, &info) && info.dli_fname != NULL) {
		fprintf(stderr, "topaz-crash: %s %p = %s+0x%lx (%s)\n", what, addr, info.dli_fname,
		    (unsigned long)((char *)addr - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	} else {
		fprintf(stderr, "topaz-crash: %s %p (no image)\n", what, addr);
	}
}

static void
topaz_crash(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = (ucontext_t *)uc_;
	void *frames[16];
	int n;

	fprintf(stderr, "topaz-crash: signal %d at address %p\n", sig, si ? si->si_addr : NULL);
	if (uc != NULL && uc->uc_mcontext != NULL) {
		topaz_where("pc", (void *)uc->uc_mcontext->__ss.__pc);
		topaz_where("lr", (void *)uc->uc_mcontext->__ss.__lr);
	}
	n = backtrace(frames, 16);
	for (int i = 0; i < n; i++) {
		topaz_where("bt", frames[i]);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
topaz_crash_handlers(void)
{
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP, SIGFPE };

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = topaz_crash;
	sa.sa_flags = SA_SIGINFO;
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		sigaction(sigs[i], &sa, NULL);
	}
}

/*
 * containermanagerd (both): they ask the user manager for the user's personas, of which this
 * system has none, again and again before giving up, and everything that needs a container
 * waits with them -- extensions (posters, widgets) are not registered at all. Told personas are
 * not supported, they do not ask. (darwin-redwood inject.c; address of this 16.7.16 cache.)
 */
#define FN_PERSONAS_ARE_SUPPORTED 0x1c4c699f8UL /* +[MCMUserIdentityCache personasAreSupported] */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static void
container_manager_fixes(void)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);
	Class cls = objc_getClass("MCMUserIdentityCache");
	Method method = cls ? class_getClassMethod(cls, sel_registerName("personasAreSupported")) : NULL;
	IMP original = method ? method_getImplementation(method) : NULL;

	if (cache == NULL || original == NULL ||
	    (uintptr_t)original != FN_PERSONAS_ARE_SUPPORTED + ((uintptr_t)cache - CACHE_BASE)) {
		fprintf(stderr, "topaz-cm: +[MCMUserIdentityCache personasAreSupported] not as expected: left alone\n");
		return;
	}
	method_setImplementation(method, (IMP)personas_are_supported);
	fprintf(stderr, "topaz-cm: container manager: no personas\n");
	FILE *f = fopen("/var/mobile/topaz-logs/inject.log", "a");      /* (launchd sends its stderr to /dev/null) */
	if (f != NULL) {
		fprintf(f, "%s[%d]: no personas\n", getprogname(), getpid());
		fclose(f);
	}
}

/*
 * lavender: boot-arg sbsnap=1 (with SpringBoard in topaz_inject's list): what SpringBoard does
 * with the snapshots of apps (the cards of the app switcher, the card of an app being closed come
 * out black on the GPU path, right with gl=0): every call of the snapshot functions of
 * QuartzCore, IOSurfaceAccelerator and vImage is written down with what the pictures hold.
 */
struct sb_vbuf { void *data; uintptr_t height, width, row; };
struct sb_interpose { const void *replacement, *replacee; };
static void (*sb_dyn_interpose)(const void *header, const struct sb_interpose *array, size_t count);
static uint32_t (*sb_surface_id)(void *);
static size_t (*sb_surface_w)(void *), (*sb_surface_h)(void *), (*sb_surface_row)(void *);
static uint32_t (*sb_surface_fmt)(void *);
static int (*sb_surface_lock)(void *, uint32_t, uint32_t *), (*sb_surface_unlock)(void *, uint32_t, uint32_t *);
static void *(*sb_surface_base)(void *);

static void
sb_look(const char *what, const uint8_t *base, size_t w, size_t h, size_t row, unsigned bpp)
{
	unsigned lit = 0, n = 0;
	uint32_t mid = 0;

	if (base == NULL || w == 0 || h == 0) {
		fprintf(stderr, "topaz-sbsnap: %s: no pixels\n", what);
		return;
	}
	for (size_t y = h / 16; y < h; y += h / 8 + 1) {
		for (size_t x = w / 16; x < w; x += w / 8 + 1) {
			const uint8_t *px = base + y * row + x * bpp;
			n++;
			lit += bpp >= 3 ? (px[0] | px[1] | px[2]) != 0 : px[0] != 0;
		}
	}
	memcpy(&mid, base + (h / 2) * row + (w / 2) * bpp, bpp < 4 ? bpp : 4);
	fprintf(stderr, "topaz-sbsnap: %s: %zux%zu row %zu: middle %08x, %u of %u samples not black\n", what, w, h, row, mid, lit, n);
}

static void
sb_surface(const char *what, void *surface)
{
	char label[160];

	if (surface == NULL || sb_surface_id == NULL) {
		fprintf(stderr, "topaz-sbsnap: %s: surface NULL\n", what);
		return;
	}
	uint32_t fmt = sb_surface_fmt(surface);
	snprintf(label, sizeof(label), "%s surface %u fmt %08x", what, sb_surface_id(surface), fmt);
	if (sb_surface_lock(surface, 1, NULL) == 0) {
		sb_look(label, sb_surface_base(surface), sb_surface_w(surface), sb_surface_h(surface), sb_surface_row(surface),
		    fmt == 0x42475241 || fmt == 0x52474241 ? 4 : 1);
		sb_surface_unlock(surface, 1, NULL);
	} else {
		fprintf(stderr, "topaz-sbsnap: %s: cannot be locked\n", label);
	}
}

typedef uintptr_t (*sb_fn8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static sb_fn8 sb_o_render_display, sb_o_capture_display, sb_o_capture_display_t, sb_o_capture_layer, sb_o_capture_layer_t,
    sb_o_capture_layer_to, sb_o_accel_create, sb_o_accel_transfer, sb_o_vscale, sb_o_vconv420;

static uintptr_t
sb_render_display(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	uintptr_t r = sb_o_render_display(a, b, c, d, e, f, g, h);
	fprintf(stderr, "topaz-sbsnap: CARenderServerRenderDisplay -> %#lx\n", (unsigned long)r);
	sb_surface("  into", (void *)c);
	return r;
}

static uintptr_t
sb_captured(const char *name, uintptr_t r)
{
	fprintf(stderr, "topaz-sbsnap: %s -> %#lx\n", name, (unsigned long)r);
	sb_surface("  captured", (void *)r);
	return r;
}
static uintptr_t
sb_capture_display(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	return sb_captured("CARenderServerCaptureDisplay", sb_o_capture_display(a, b, c, d, e, f, g, h));
}
static uintptr_t
sb_capture_display_t(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	return sb_captured("CARenderServerCaptureDisplayWithTransform", sb_o_capture_display_t(a, b, c, d, e, f, g, h));
}
static uintptr_t
sb_capture_layer(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	return sb_captured("CARenderServerCaptureLayer", sb_o_capture_layer(a, b, c, d, e, f, g, h));
}
static uintptr_t
sb_capture_layer_t(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	return sb_captured("CARenderServerCaptureLayerWithTransform", sb_o_capture_layer_t(a, b, c, d, e, f, g, h));
}
static uintptr_t
sb_capture_layer_to(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	return sb_captured("CARenderServerCaptureLayerWithTransformAndTimeOffset", sb_o_capture_layer_to(a, b, c, d, e, f, g, h));
}

static uintptr_t
sb_accel_create(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	uintptr_t r = sb_o_accel_create(a, b, c, d, e, f, g, h);
	fprintf(stderr, "topaz-sbsnap: IOSurfaceAcceleratorCreate -> %#lx\n", (unsigned long)r);
	return r;
}
static uintptr_t
sb_accel_transfer(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	sb_surface("IOSurfaceAcceleratorTransferSurface from", (void *)b);
	uintptr_t r = sb_o_accel_transfer(a, b, c, d, e, f, g, h);
	fprintf(stderr, "topaz-sbsnap: IOSurfaceAcceleratorTransferSurface -> %#lx\n", (unsigned long)r);
	sb_surface("  to", (void *)c);
	return r;
}
static uintptr_t
sb_vscale(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	const struct sb_vbuf *src = (const struct sb_vbuf *)a, *dst = (const struct sb_vbuf *)b;
	if (src != NULL) {
		sb_look("vImageScale_ARGB8888 from", src->data, src->width, src->height, src->row, 4);
	}
	uintptr_t r = sb_o_vscale(a, b, c, d, e, f, g, h);
	if (dst != NULL) {
		sb_look("  to", dst->data, dst->width, dst->height, dst->row, 4);
	}
	return r;
}
static uintptr_t
sb_vconv420(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h)
{
	const struct sb_vbuf *src = (const struct sb_vbuf *)a;
	if (src != NULL) {
		sb_look("vImageConvert_ARGB8888To420Yp8_CbCr8 from", src->data, src->width, src->height, src->row, 4);
	}
	return sb_o_vconv420(a, b, c, d, e, f, g, h);
}

static void
sb_snapshot_hooks(void)
{
	struct { const char *name; void *mine; sb_fn8 *orig; } hooks[] = {
		{ "CARenderServerRenderDisplay", (void *)sb_render_display, &sb_o_render_display },
		{ "CARenderServerCaptureDisplay", (void *)sb_capture_display, &sb_o_capture_display },
		{ "CARenderServerCaptureDisplayWithTransform", (void *)sb_capture_display_t, &sb_o_capture_display_t },
		{ "CARenderServerCaptureLayer", (void *)sb_capture_layer, &sb_o_capture_layer },
		{ "CARenderServerCaptureLayerWithTransform", (void *)sb_capture_layer_t, &sb_o_capture_layer_t },
		{ "CARenderServerCaptureLayerWithTransformAndTimeOffset", (void *)sb_capture_layer_to, &sb_o_capture_layer_to },
		{ "IOSurfaceAcceleratorCreate", (void *)sb_accel_create, &sb_o_accel_create },
		{ "IOSurfaceAcceleratorTransferSurface", (void *)sb_accel_transfer, &sb_o_accel_transfer },
		{ "vImageScale_ARGB8888", (void *)sb_vscale, &sb_o_vscale },
		{ "vImageConvert_ARGB8888To420Yp8_CbCr8", (void *)sb_vconv420, &sb_o_vconv420 },
	};
	struct sb_interpose tuples[16];
	size_t n = 0;

	dlopen("/System/Library/PrivateFrameworks/IOSurfaceAccelerator.framework/IOSurfaceAccelerator", RTLD_NOW);
	dlopen("/System/Library/Frameworks/Accelerate.framework/Accelerate", RTLD_NOW);
	dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW);
	dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
	sb_dyn_interpose = (void (*)(const void *, const struct sb_interpose *, size_t))dlsym(RTLD_DEFAULT, "dyld_dynamic_interpose");
	sb_surface_id = (uint32_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetID");
	sb_surface_w = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetWidth");
	sb_surface_h = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetHeight");
	sb_surface_row = (size_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetBytesPerRow");
	sb_surface_fmt = (uint32_t (*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetPixelFormat");
	sb_surface_lock = (int (*)(void *, uint32_t, uint32_t *))dlsym(RTLD_DEFAULT, "IOSurfaceLock");
	sb_surface_unlock = (int (*)(void *, uint32_t, uint32_t *))dlsym(RTLD_DEFAULT, "IOSurfaceUnlock");
	sb_surface_base = (void *(*)(void *))dlsym(RTLD_DEFAULT, "IOSurfaceGetBaseAddress");
	for (size_t i = 0; i < sizeof(hooks) / sizeof(hooks[0]); i++) {
		void *at = dlsym(RTLD_DEFAULT, hooks[i].name);
		if (at != NULL) {
			*hooks[i].orig = (sb_fn8)at;
			tuples[n].replacement = hooks[i].mine;
			tuples[n].replacee = at;
			n++;
		} else {
			fprintf(stderr, "topaz-sbsnap: no %s\n", hooks[i].name);
		}
	}
	if (sb_dyn_interpose == NULL || sb_surface_lock == NULL) {
		fprintf(stderr, "topaz-sbsnap: no dyld_dynamic_interpose (%p) or IOSurface (%p): nothing hooked\n", (void *)sb_dyn_interpose,
		    (void *)sb_surface_lock);
		return;
	}
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		sb_dyn_interpose(_dyld_get_image_header(i), tuples, n);
	}
	fprintf(stderr, "topaz-sbsnap: %zu snapshot functions hooked in %u images\n", n, _dyld_image_count());
}

static void exc_said(id exception);
static int exc_count;
static id
exc_thrown(id exception)
{
	if (exc_count++ < 6) {
		exc_said(exception);
	}
	return exception;
}

/* lavender: PLPrivacy's answer for the photo library's own daemon: allowed (see below) */
static long
photos_allowed(id self, SEL cmd, long scope)
{
	(void)self; (void)cmd; (void)scope;
	return 0;
}

/* lavender: what this process can see of its own entitlements (the AMFI stand-in's answers) */
static void
entitlements_self_check(void)
{
	extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
	uint8_t buf[8192];
	uint32_t flags = 0;
	int r0 = csops(getpid(), 0, &flags, sizeof(flags));                 /* CS_OPS_STATUS */
	memset(buf, 0, sizeof(buf));
	int r7 = csops(getpid(), 7, buf, sizeof(buf));                      /* CS_OPS_ENTITLEMENTS_BLOB (XML) */
	uint32_t xlen = r7 == 0 ? ntohl(*(uint32_t *)(buf + 4)) : 0;
	int has_tcc_xml = r7 == 0 && memmem(buf, sizeof(buf), "com.apple.private.tcc.allow", 27) != NULL;
	memset(buf, 0, sizeof(buf));
	int r16 = csops(getpid(), 16, buf, sizeof(buf));                    /* CS_OPS_DER_ENTITLEMENTS_BLOB */
	uint32_t dlen = r16 == 0 ? ntohl(*(uint32_t *)(buf + 4)) : 0;
	int has_tcc_der = r16 == 0 && memmem(buf, sizeof(buf), "com.apple.private.tcc.allow", 27) != NULL;
	fprintf(stderr, "topaz-ent: %s: csflags %#x (%d); XML entitlements %d len %u tcc %d; DER %d (errno %d) len %u tcc %d\n",
	    getprogname(), flags, r0, r7, xlen, has_tcc_xml, r16, r16 ? errno : 0, dlen, has_tcc_der);

	void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW);
	void *(*task_self)(void *) = sec ? (void *(*)(void *))dlsym(sec, "SecTaskCreateFromSelf") : NULL;
	void *(*task_value)(void *, CFStringRef, void *) = sec ? (void *(*)(void *, CFStringRef, void *))dlsym(sec, "SecTaskCopyValueForEntitlement") : NULL;
	if (task_self != NULL && task_value != NULL) {
		void *task = task_self(NULL);
		CFTypeRef v = task ? task_value(task, CFSTR("com.apple.private.tcc.allow"), NULL) : NULL;
		char text[200] = "none";
		if (v != NULL) {
			CFStringRef d = CFCopyDescription(v);
			CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(d);
			CFRelease(v);
		}
		fprintf(stderr, "topaz-ent: %s: SecTask com.apple.private.tcc.allow = %s\n", getprogname(), text);
		if (task) {
			CFRelease(task);
		}
	}
}

static void
photos_privacy_fix(void)
{
	dlopen("/System/Library/PrivateFrameworks/PhotoLibraryServicesCore.framework/PhotoLibraryServicesCore", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/PhotoLibraryServices.framework/PhotoLibraryServices", RTLD_NOW);
	Class privacy = objc_getClass("PLPrivacy");
	Method m = privacy ? class_getInstanceMethod(privacy, sel_registerName("photosAccessAllowedWithScope:")) : NULL;

	if (m != NULL) {
		method_setImplementation(m, (IMP)photos_allowed);
	}
	fprintf(stderr, "topaz-exc: %s: -[PLPrivacy photosAccessAllowedWithScope:] %s\n", getprogname(), m ? "answers allowed" : "not found");
}

/* lavender: a daemon that dies of an uncaught exception (assetsd: Photos black) says why on stderr */
static void
exc_said(id exception)
{
	id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id name = send(exception, sel_registerName("name")), reason = send(exception, sel_registerName("reason"));
	id stack = send(exception, sel_registerName("callStackSymbols"));
	id text = stack != NULL ? send(stack, sel_registerName("description")) : NULL;

	fprintf(stderr, "topaz-exc: %s[%d]: %s: %s\n", getprogname(), getpid(), name ? utf8(name, sel_registerName("UTF8String")) : "?",
	    reason ? utf8(reason, sel_registerName("UTF8String")) : "?");
	if (text != NULL) {
		fprintf(stderr, "topaz-exc: %s\n", utf8(text, sel_registerName("UTF8String")));
	}
}

__attribute__((constructor))
static void
topaz_inject(void)
{
	const char *name = getprogname();
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	io_service_t gpu;
	void *lib;

	if (name != NULL && strncmp(name, "containermanagerd", 17) == 0) {
		container_manager_fixes();
		return;
	}
	if (name != NULL && (strcmp(name, "assetsd") == 0 || strcmp(name, "AegirPoster") == 0 || strcmp(name, "MobileSlideShow") == 0)) {
		int console = open("/dev/console", O_WRONLY);     /* (launchd gives these daemons no stderr) */
		if (console >= 0) {
			dup2(console, 2);
			close(console);
		}
		objc_setUncaughtExceptionHandler((objc_uncaught_exception_handler)exc_said);
		objc_setExceptionPreprocessor((objc_exception_preprocessor)exc_thrown);     /* (CoreFoundation replaces the handler) */
		/*
		 * lavender: assetsd asks TCC (PLPrivacy) whether it may use the photo library and throws
		 * "assetsd requires TCCPhotosService access" unless the answer is "allowed" - tccd here
		 * does not see its private TCC entitlement (the kernel's AMFI stand-in), so Photos stayed
		 * black. The daemon of the photo library is allowed its own library.
		 */
		entitlements_self_check();
		photos_privacy_fix();
		fprintf(stderr, "topaz-exc: %s: uncaught exceptions are told\n", name);
		return;
	}
	if (name != NULL && strcmp(name, "SpringBoard") == 0) {
		char sargs[1024] = "";
		size_t ssize = sizeof(sargs) - 1;

		if (sysctlbyname("kern.bootargs", sargs, &ssize, NULL, 0) == 0 && strstr(sargs, "sbsnap=1") != NULL) {
			sb_snapshot_hooks();
		}
		return;
	}
	if (name == NULL || strcmp(name, "backboardd") != 0 || access(TOPAZ_GL, R_OK) != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		args[0] = 0;
	}
	/*
	 * lavender: boot-arg lv_photoreset=1: the photo library aside (assetsd threw on every start
	 * after a day of forced restarts, Photos stayed black): /var/mobile/Media/PhotoData is renamed
	 * PhotoData.bak-SECONDS, nothing is deleted; assetsd makes a new one.
	 */
	/*
	 * lavender: boot-arg lv_tccreset=1: the privacy database aside (assetsd: "requires
	 * TCCPhotosService access" on every start, Photos black): TCC.db (and its -wal/-shm) renamed
	 * .bak-SECONDS; tccd makes a new one. lv_photorestore=NAME: PhotoData.bak-NAME back in place.
	 */
	if (strstr(args, "lv_tccreset=1") != NULL) {
		const char *parts[] = { "", "-wal", "-shm" };
		long now = (long)time(NULL);
		for (int i = 0; i < 3; i++) {
			char from[96], to[112];
			snprintf(from, sizeof(from), "/var/mobile/Library/TCC/TCC.db%s", parts[i]);
			snprintf(to, sizeof(to), "%s.bak-%ld", from, now);
			if (access(from, F_OK) == 0) {
				int r = rename(from, to);
				fprintf(stderr, "topaz-inject: %s -> %s: %s\n", from, to, r == 0 ? "done" : strerror(errno));
			}
		}
	}
	const char *restore = strstr(args, "lv_photorestore=");
	if (restore != NULL) {
		char from[96], to[96];
		size_t n = strcspn(restore + 16, " ");
		snprintf(from, sizeof(from), "/var/mobile/Media/PhotoData.bak-%.*s", (int)(n < 20 ? n : 20), restore + 16);
		if (access(from, F_OK) == 0) {
			snprintf(to, sizeof(to), "/var/mobile/Media/PhotoData.new-%ld", (long)time(NULL));
			if (access("/var/mobile/Media/PhotoData", F_OK) == 0) {
				rename("/var/mobile/Media/PhotoData", to);
			}
			int r = rename(from, "/var/mobile/Media/PhotoData");
			fprintf(stderr, "topaz-inject: photo library %s back in place: %s\n", from, r == 0 ? "done" : strerror(errno));
		}
	}
	if (strstr(args, "lv_photoreset=1") != NULL && access("/var/mobile/Media/PhotoData", F_OK) == 0) {
		char to[96];
		snprintf(to, sizeof(to), "/var/mobile/Media/PhotoData.bak-%ld", (long)time(NULL));
		int r = rename("/var/mobile/Media/PhotoData", to);
		fprintf(stderr, "topaz-inject: photo library moved to %s: %s\n", to, r == 0 ? "done" : strerror(errno));
	}
	/*
	 * lavender: boot-arg caenv=NAME=VALUE (as many as wanted) into the window server's
	 * environment before CoreAnimation starts: its own switches (CA_DISABLE_DIRTY_REGIONS=1,
	 * CA_DISABLE_BACKDROP_CACHE=1, CA_PRINT_BACKDROPS=1 ...; strings in the shared cache).
	 * libtopaz-gl's glenv= comes too late for those that are read once at start.
	 */
	for (const char *argument = args; (argument = strstr(argument, "caenv=")) != NULL; argument += 6) {
		char setting[128];
		size_t length = strcspn(argument + 6, " ");
		char *equals;

		if ((argument != args && argument[-1] != ' ') || length == 0 || length >= sizeof(setting)) {
			continue;
		}
		memcpy(setting, argument + 6, length);
		setting[length] = 0;
		if ((equals = strchr(setting, '=')) != NULL) {
			*equals = 0;
			setenv(setting, equals + 1, 1);
			fprintf(stderr, "topaz-inject: caenv %s=%s\n", setting, equals + 1);
		}
	}
	/*
	 * lavender: Mesa draws straight into memory (FD_MESA_DEBUG=sysmem) unless told otherwise:
	 * CoreAnimation's ~20 flushes a frame each cost freedreno a whole tiled (GMEM) pass on the
	 * processor; without tiles home went 36 -> 45 fps, Settings 26 -> 29 (2026-11-22). Boot-arg
	 * glgmem=1: tiles again.
	 */
	if (strstr(args, "glgmem=1") == NULL && getenv("FD_MESA_DEBUG") == NULL) {
		setenv("FD_MESA_DEBUG", "sysmem", 1);
		fprintf(stderr, "topaz-inject: FD_MESA_DEBUG=sysmem (glgmem=1: tiles)\n");
	}
	/*
	 * lavender: Mesa's driver half (state emit, draw packets, submissions) on a thread of its own
	 * (u_threaded_context, freedreno-texbarrier.patch): Settings 38 -> 44 fps. gltc=0: one thread.
	 */
	if (strstr(args, "gltc=0") == NULL && getenv("FD_TC") == NULL) {
		setenv("FD_TC", "1", 1);
		fprintf(stderr, "topaz-inject: FD_TC=1 (gltc=0: Mesa on one thread)\n");
	}
	if (strstr(args, " gl=0") != NULL) {
		syslog(LOG_NOTICE, "topaz: gl=0: CoreAnimation keeps its software renderer");
		return;
	}
	/* the kernel starts the GPU once the root filesystem is there: give it a moment */
	for (int i = 0; i < 50; i++) {
		gpu = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodGPU"));
		if (gpu != 0) {
			break;
		}
		usleep(100000);
	}
	if (gpu == 0) {
		syslog(LOG_NOTICE, "topaz: no GPU service: software renderer");
		return;
	}
	IOObjectRelease(gpu);
	topaz_crash_handlers();
	lib = dlopen(TOPAZ_GL, RTLD_NOW);
	gl_hooks(lib);
	fprintf(stderr, "topaz-crash: handlers set, GL %s\n", lib ? "loaded" : dlerror());
	syslog(LOG_NOTICE, "topaz: %s %s", TOPAZ_GL, lib ? "loaded" : dlerror());
}
