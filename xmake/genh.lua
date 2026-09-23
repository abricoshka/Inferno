local osdep_prefix = [[
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stddef.h>
#include <sys/types.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
]]

local function define(name)
	set_configvar(name, "", {quote = false})
end

local function probe(name, opt, setup)
	interp_save_scope()
	option("__" .. name)
		set_showmenu(false)
		add_defines("_GNU_SOURCE")
		if opt.configvar ~= false then
			define(name)
		end
		if opt.links then
			add_links(opt.links)
		end
		if opt.cflags then
			add_cflags(opt.cflags)
		end
		setup()
	option_end()
	interp_restore_scope()
	add_options("__" .. name)
end

local function probe_header(name, header)
	probe(name, {}, function ()
		add_cincludes(header)
	end)
end

local function probe_function(name, func, includes, links)
	probe(name, {links = links}, function ()
		add_cfuncs(func)
		add_cincludes(includes)
	end)
end

local function probe_symbol(name, includes, symbols)
	local body = {}
	for _, symbol in ipairs(symbols) do
		table.insert(body, ("#ifndef %s\n    (void) %s;\n#endif"):format(symbol, symbol))
	end
	probe(name, {}, function ()
		add_cincludes(includes)
		add_csnippets(name, "void probe(void);\nvoid probe(void)\n{\n" .. table.concat(body, "\n") .. "\n}")
	end)
end

local function probe_compiles(name, code, opt)
	opt = opt or {}
	probe(name, {configvar = opt.configvar, cflags = opt.cflags}, function ()
		add_csnippets(name, code)
	end)
end

local function probe_links(name, code, opt)
	opt = opt or {}
	probe(name, {configvar = opt.configvar, cflags = opt.cflags, links = "pthread"}, function ()
		add_csnippets(name, code)
	end)
end


target("genh")
	set_kind("phony")
	add_rules("inferno.genh")
	add_packages("glib", "mlib", "keycodemapdb", {public = true})
	add_includedirs(inferno_gendir, path.join(inferno_gendir, "qapi"), path.join(inferno_gendir, "trace"),
		path.join(inferno_gendir, "ui"), {public = true})
	if is_plat("linux") then
		add_sysincludedirs(path.join(inferno_gendir, "linux-headers"), {public = true})
	end
	set_values("inferno.target", inferno_target)
	set_values("inferno.cpu", inferno_cpu)
	set_values("inferno.coroutine_backend", inferno_coroutine_backend)
	if (inferno_have_vnc or inferno_have_curses) and not has_package("xkbcommon") then
		set_values("inferno.keymaps_missing", "1")
	end
	if inferno_stap then
		set_values("inferno.traceable", "inferno-aarch64")
		if has_config("tools") then
			add_values("inferno.traceable", "inferno-img")
		end
	end
	if inferno_linux_arch then
		set_values("inferno.linux_arch", inferno_linux_arch)
	end
	set_values("inferno.trace_dirs", inferno_subdirs)

	set_configdir(inferno_gendir)
	add_configfiles("../configs/config-host.h.in")
	add_configfiles("../configs/config-target.h.in", {filename = inferno_target .. "-config-target.h"})
	add_configfiles("../configs/config-devices.h.in", {filename = inferno_target .. "-config-devices.h"})

	add_options("tcg", "tcg_interpreter", "hvf", "kvm", "debug_tcg", "debug_mutex", "debug_graph_lock",
		"debug_stack_usage", "qom_cast_debug", "coroutine_pool", "membarrier", "gtk_clipboard",
		"__io_prep_pwritev2", "__rbd_namespace_exists")
	add_options(table.unpack(inferno_devices))
	if has_config("seccomp") then
		add_options("__seccomp_sysrawrc")
	end
	if has_config("linux_io_uring") then
		add_options("__io_uring_prep_writev2")
	end

	set_configvar("HOST_" .. inferno_host_arch:upper(), 1)
	define("CONFIG_ASAN_IFACE_FIBER")
	if not has_config("__nettle_xts") then
		define("CONFIG_QEMU_PRIVATE_XTS")
	end
	if is_plat("mingw") then
		define("CONFIG_WIN32")
		probe_header("HAVE_AFUNIX_H", "afunix.h")
		probe_links("HAVE__LOCK_FILE", [[
#include <stdio.h>
int main(void) {
    _lock_file(NULL);
    _unlock_file(NULL);
    return 0;
}]])
		probe_function("CONFIG_ALIGNED_MALLOC", "_aligned_malloc", "malloc.h")
		probe_links("mingw_setjmp", [[
#include <setjmp.h>
int main(void) {
    extern int __mingw_setjmp(jmp_buf);
    extern void __attribute__((noreturn)) __mingw_longjmp(jmp_buf, int);
    jmp_buf env;
    __mingw_setjmp(env);
    __mingw_longjmp(env, 0);
}]], {configvar = false})
	else
		define("CONFIG_POSIX")
	end
	if is_plat("linux") then
		define("CONFIG_LINUX")
	elseif is_plat("macosx", "bsd") then
		define("CONFIG_BSD")
	end
	if is_plat("macosx") then
		define("CONFIG_DARWIN")
		probe_header("HAVE_HOST_BLOCK_DEVICE", "IOKit/storage/IOMedia.h")
	else
		define("HAVE_HOST_BLOCK_DEVICE")
	end
	for _, backend in ipairs(inferno_trace_backends) do
		define("CONFIG_TRACE_" .. backend:upper())
	end

	local audio = {
		alsa = has_package("alsa"),
		coreaudio = has_config("coreaudio"),
		dsound = has_config("dsound"),
		jack = has_package("jack"),
		oss = has_config("oss"),
		pa = has_package("pa"),
		pipewire = has_package("pipewire"),
		sdl = has_package("sdl"),
		sndio = has_package("sndio"),
	}
	for driver, found in pairs(audio) do
		if found then
			define("CONFIG_AUDIO_" .. driver:upper())
		end
	end
	local audio_default = {}
	for _, driver in ipairs(is_plat("linux") and {"pa", "coreaudio", "dsound", "sndio", "oss"} or
			{"pa", "coreaudio", "dsound", "sndio", "oss", "sdl"}) do
		if audio[driver] then
			table.insert(audio_default, driver)
		end
	end
	set_configvar("CONFIG_AUDIO_DRIVERS", '"' .. table.concat(audio_default, '", "') .. '", ', {quote = false})

	for name, found in pairs({
		CONFIG_AVAHI = has_package("avahi") and has_package("avahi_glib"),
		CONFIG_BLKIO = has_package("blkio"),
		CONFIG_COCOA = has_config("cocoa"),
		CONFIG_CURSES = inferno_have_curses,
		CONFIG_FUSE = has_package("fuse"),
		CONFIG_GNUTLS = has_package("gnutls"),
		CONFIG_GTK = has_package("gtk"),
		CONFIG_HOGWEED = inferno_have_hogweed,
		CONFIG_LIBCAP_NG = has_config("cap_ng"),
		CONFIG_LIBDAXCTL = has_package("libdaxctl"),
		CONFIG_LIBPMEM = has_package("libpmem"),
		CONFIG_LINUX_AIO = has_config("linux_aio"),
		CONFIG_LINUX_IO_URING = inferno_have_io_uring,
		CONFIG_PNG = has_package("png"),
		CONFIG_SDL = has_package("sdl"),
		CONFIG_SDL_IMAGE = has_package("sdl") and has_package("sdl_image"),
		CONFIG_SECRET_KEYRING = has_config("__keyring"),
		CONFIG_SECCOMP = has_package("seccomp"),
		CONFIG_SLIRP = has_package("slirp"),
		CONFIG_VDE = has_config("vde"),
		CONFIG_VMNET = has_config("vmnet"),
		CONFIG_VNC = inferno_have_vnc,
		CONFIG_VNC_JPEG = inferno_have_vnc and has_package("vnc_jpeg"),
		CONFIG_VNC_SASL = has_config("vnc_sasl"),
		CONFIG_VTE = has_package("gtk") and has_package("vte"),
		CONFIG_X11 = has_package("x11"),
		CONFIG_ZSTD = has_package("zstd"),
	}) do
		if found then
			define(name)
		end
	end

	probe_header("CONFIG_EPOLL", "sys/epoll.h")
	probe_header("CONFIG_VALGRIND_H", "valgrind/valgrind.h")
	probe_header("HAVE_PTY_H", "pty.h")
	probe_header("HAVE_SYS_DISK_H", "sys/disk.h")

	probe_function("CONFIG_ACCEPT4", "accept4", "sys/socket.h")
	probe_function("CONFIG_CLOSE_RANGE", "close_range", "unistd.h")
	probe_function("CONFIG_FALLOCATE", "fallocate", "fcntl.h")
	probe_function("CONFIG_GETTID", "gettid", "unistd.h")
	probe_function("CONFIG_MALLOC_TRIM", "malloc_trim", "malloc.h")
	probe_function("CONFIG_MEMALIGN", "memalign", "malloc.h")
	probe_function("CONFIG_POSIX_FALLOCATE", "posix_fallocate", "fcntl.h")
	probe_function("CONFIG_POSIX_MEMALIGN", "posix_memalign", "stdlib.h")
	probe_function("CONFIG_PPOLL", "ppoll", "poll.h")
	probe_function("CONFIG_PREADV", "preadv", "sys/uio.h")
	probe_function("CONFIG_VALLOC", "valloc", "stdlib.h")
	probe_function("HAVE_COPY_FILE_RANGE", "copy_file_range", "unistd.h")
	probe_function("HAVE_OPENPTY", "openpty", is_plat("macosx") and "util.h" or "pty.h", "util")
	probe_function("HAVE_STRCHRNUL", "strchrnul", "string.h")
	probe_function("HAVE_SYSTEM_FUNCTION", "system", "stdlib.h")

	probe_symbol("CONFIG_AF_VSOCK", {"sys/socket.h", "linux/vm_sockets.h"}, {"AF_VSOCK"})
	probe_symbol("CONFIG_BLKZONED", "linux/blkzoned.h", {"BLKOPENZONE"})
	probe_symbol("CONFIG_EPOLL_CREATE1", "sys/epoll.h", {"epoll_create1"})
	probe_symbol("CONFIG_FALLOCATE_PUNCH_HOLE", "linux/falloc.h", {"FALLOC_FL_PUNCH_HOLE", "FALLOC_FL_KEEP_SIZE"})
	probe_symbol("CONFIG_FALLOCATE_ZERO_RANGE", "linux/falloc.h", {"FALLOC_FL_ZERO_RANGE"})
	probe_symbol("CONFIG_GETRANDOM", "sys/random.h", {"getrandom", "GRND_NONBLOCK"})
	probe_symbol("CONFIG_INOTIFY1", "sys/inotify.h", {"inotify_init1"})
	probe_symbol("CONFIG_PRCTL_PR_SET_TIMERSLACK", "sys/prctl.h", {"PR_SET_TIMERSLACK"})
	probe_symbol("CONFIG_SYSMACROS", "sys/sysmacros.h", {"makedev"})
	probe_symbol("HAVE_IPPROTO_MPTCP", "netinet/in.h", {"IPPROTO_MPTCP"})
	probe_symbol("HAVE_OPTRESET", "getopt.h", {"optreset"})
	local tcp_header = is_plat("mingw") and "ws2tcpip.h" or "netinet/tcp.h"
	probe_symbol("HAVE_TCP_KEEPCNT", tcp_header, {"TCP_KEEPCNT"})
	probe_symbol("HAVE_TCP_KEEPINTVL", tcp_header, {"TCP_KEEPINTVL"})
	probe_compiles("HAVE_TCP_KEEPIDLE", "#include <" .. tcp_header .. ">\n" .. [[
#if !defined(TCP_KEEPIDLE) && !defined(TCP_KEEPALIVE)
#error
#endif
int main(void) { return 0; }]])
	probe_compiles("HAVE_BLK_ZONE_REP_CAPACITY", [[
#include <linux/blkzoned.h>
void probe(void);
void probe(void) { struct blk_zone foo; (void) foo.capacity; }]])
	probe("CONFIG_IOVEC", {}, function ()
		add_ctypes("struct iovec")
		add_cincludes("sys/uio.h")
	end)
	probe("CONFIG_L2TPV3", {}, function ()
		add_ctypes("struct mmsghdr")
		add_cincludes("stddef.h", "sys/types.h", "string.h", "limits.h", "unistd.h", "time.h", "errno.h", "fcntl.h",
			"sys/socket.h", "linux/ip.h")
	end)

	probe_links("CONFIG_EVENTFD", [[
#include <sys/eventfd.h>
int main(void) { return eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); }]])
	probe_links("CONFIG_FDATASYNC", osdep_prefix .. [[
int main(void) {
#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
    return fdatasync(0);
#else
#error Not supported
#endif
}]])
	probe_links("CONFIG_MADVISE", osdep_prefix .. [[
#include <sys/mman.h>
int main(void) { return madvise(NULL, 0, MADV_DONTNEED); }]])
	probe_links("HAVE_MADVISE_WITHOUT_PROTOTYPE", osdep_prefix .. [[
#include <sys/mman.h>
extern int madvise(int);
int main(void) { return madvise(0); }]])
	probe_links("CONFIG_MEMFD", osdep_prefix .. [[
#include <sys/mman.h>
int main(void) { return memfd_create("foo", MFD_ALLOW_SEALING); }]])
	if not is_plat("macosx") then
		probe_links("CONFIG_POSIX_MADVISE", osdep_prefix .. [[
#include <sys/mman.h>
int main(void) { return posix_madvise(NULL, 0, POSIX_MADV_DONTNEED); }]])
	end
	probe_links("CONFIG_PTHREAD_SETNAME_NP_W_TID", osdep_prefix .. [[
#include <pthread.h>
static void *f(void *p) { return NULL; }
int main(void)
{
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    pthread_setname_np(thread, "ChefKiss Inferno");
    return 0;
}]])
	probe_links("CONFIG_PTHREAD_SETNAME_NP_WO_TID", osdep_prefix .. [[
#include <pthread.h>
static void *f(void *p) { pthread_setname_np("ChefKiss Inferno"); return NULL; }
int main(void)
{
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    return 0;
}]])
	probe_links("CONFIG_PTHREAD_SET_NAME_NP", osdep_prefix .. [[
#include <pthread.h>
#include <pthread_np.h>
static void *f(void *p) { return NULL; }
int main(void)
{
    pthread_t thread;
    pthread_create(&thread, 0, f, 0);
    pthread_set_name_np(thread, "ChefKiss Inferno");
    return 0;
}]])
	probe_links("CONFIG_PTHREAD_CONDATTR_SETCLOCK", osdep_prefix .. [[
#include <pthread.h>
int main(void)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    return 0;
}]])
	probe_links("CONFIG_PTHREAD_AFFINITY_NP", osdep_prefix .. [[
#include <pthread.h>
static void *f(void *p) { return NULL; }
int main(void)
{
    int setsize = CPU_ALLOC_SIZE(64);
    pthread_t thread;
    cpu_set_t *cpuset;
    pthread_create(&thread, 0, f, 0);
    cpuset = CPU_ALLOC(64);
    CPU_ZERO_S(setsize, cpuset);
    pthread_setaffinity_np(thread, setsize, cpuset);
    pthread_getaffinity_np(thread, setsize, cpuset);
    CPU_FREE(cpuset);
    return 0;
}]])
	probe_links("CONFIG_SIGNALFD", osdep_prefix .. [[
#include <sys/signalfd.h>
int main(void) { return signalfd(-1, NULL, SFD_CLOEXEC); }]])
	probe_links("HAVE_MLOCKALL", osdep_prefix .. [[
#include <sys/mman.h>
int main(void) { return mlockall(MCL_FUTURE); }]])
	probe_links("HAVE_MLOCK_ONFAULT", osdep_prefix .. [[
#include <sys/mman.h>
int main(void) { return mlockall(MCL_FUTURE | MCL_ONFAULT); }]])
	probe_links("CONFIG_GETAUXVAL", osdep_prefix .. [[
#include <sys/auxv.h>
int main(void) { return getauxval(AT_HWCAP) == 0; }]])
	probe_links("CONFIG_ELF_AUX_INFO", osdep_prefix .. [[
#include <sys/auxv.h>
int main(void) {
    unsigned long hwcap = 0;
    elf_aux_info(AT_HWCAP, &hwcap, sizeof(hwcap));
    return hwcap;
}]])

	probe_links("CONFIG_ATOMIC64", [[
#include <stdint.h>
int main(void)
{
    uint64_t x = 0, y = 0;
    y = __atomic_load_n(&x, __ATOMIC_RELAXED);
    __atomic_store_n(&x, y, __ATOMIC_RELAXED);
    __atomic_compare_exchange_n(&x, &y, x, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    __atomic_exchange_n(&x, y, __ATOMIC_RELAXED);
    __atomic_fetch_add(&x, y, __ATOMIC_RELAXED);
    return 0;
}]], {cflags = inferno_isa_flags})
	if not is_plat("wasm") then
		probe_compiles("CONFIG_INT128_TYPE", [[
__int128_t a;
__uint128_t b;
int main(void) { b = a; }]])
	end
	probe_links("CONFIG_INT128", [[
__int128_t a;
__uint128_t b;
int main (void) {
    a = a + b;
    b = a * b;
    a = a * a;
    return 0;
}]])
	local atomic_test_128 = [[
int main(int ac, char **av) {
    __uint128_t *p = __builtin_assume_aligned(av[ac - 1], 16);
    p[1] = __atomic_load_n(&p[0], __ATOMIC_RELAXED);
    __atomic_store_n(&p[2], p[3], __ATOMIC_RELAXED);
    __atomic_compare_exchange_n(&p[4], &p[5], p[6], 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    return 0;
}]]
	probe_links("CONFIG_ATOMIC128", atomic_test_128, {configvar = false, cflags = inferno_isa_flags})
	probe_links("CONFIG_ATOMIC128_OPT", '__attribute__((optimize("O1")))' .. atomic_test_128,
		{configvar = false, cflags = inferno_isa_flags})
	probe_links("CONFIG_CMPXCHG128", [[
int main(void)
{
    __uint128_t x = 0, y = 0;
    __sync_val_compare_and_swap_16(&x, y, x);
    return 0;
}]], {configvar = false, cflags = inferno_isa_flags})
	if has_config("__CONFIG_INT128_TYPE") then
		if has_config("__CONFIG_ATOMIC128") then
			define("CONFIG_ATOMIC128")
		elseif has_config("__CONFIG_ATOMIC128_OPT") then
			define("CONFIG_ATOMIC128_OPT")
		elseif has_config("__CONFIG_CMPXCHG128") then
			define("CONFIG_CMPXCHG128")
		end
	end

	probe_links("CONFIG_CPUID_H", [[
#include <cpuid.h>
int main(void) {
    unsigned a, b, c, d;
    unsigned max = __get_cpuid_max(0, 0);

    if (max >= 1) {
        __cpuid(1, a, b, c, d);
    }

    if (max >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
    }

    return 0;
}]])
	probe_links("CONFIG_AVX2_OPT", [[
#include <immintrin.h>
static int __attribute__((target("avx2"))) bar(void *a) {
    __m256i x = *(__m256i *)a;
    return _mm256_testz_si256(x, x);
}
int main(int argc, char *argv[]) { return bar(argv[argc - 1]); }]], {configvar = false})
	probe_links("avx512bw", [[
#include <immintrin.h>
static int __attribute__((target("avx512bw"))) bar(void *a) {
    __m512i *x = a;
    __m512i res= _mm512_abs_epi8(*x);
    return res[1];
}
int main(int argc, char *argv[]) { return bar(argv[0]); }]], {configvar = false})
	if has_config("__CONFIG_CPUID_H") and has_config("__CONFIG_AVX2_OPT") then
		define("CONFIG_AVX2_OPT")
	end
	probe_compiles("CONFIG_ARM_AES_BUILTIN", [[
#include <arm_neon.h>
#ifndef __ARM_FEATURE_AES
__attribute__((target("+crypto")))
#endif
void foo(uint8x16_t *p);
void foo(uint8x16_t *p) { *p = vaesmcq_u8(*p); }]])
	probe_compiles("SIZE_MAX_PRINTF", [[
#include <stdint.h>
#include <stdio.h>
int main(void) {
    return printf("%zu", SIZE_MAX);
}]], {configvar = false, cflags = "-Werror"})
	if not has_config("__SIZE_MAX_PRINTF") then
		define("HAVE_BROKEN_SIZE_MAX")
	end

	probe_compiles("CONFIG_NETMAP", [[
#include <inttypes.h>
#include <net/if.h>
#include <net/netmap.h>
#include <net/netmap_user.h>
#if (NETMAP_API < 11) || (NETMAP_API > 15)
#error
#endif
int main(void) { return 0; }]])
	if inferno_cpu == "x86" then
		probe_links("i386_sync", [[
static int sfaa(int *ptr)
{
    return __sync_fetch_and_and(ptr, 0);
}

int main(void)
{
    int val = 42;
    val = __sync_val_compare_and_swap(&val, 0, 1);
    sfaa(&val);
    return val;
}]], {configvar = false})
	end
	if not is_plat("mingw", "wasm", "macosx") then
		probe_links("ucontext", [[
#include <ucontext.h>
#ifdef __stub_makecontext
#error Ignoring glibc stub makecontext which will always fail
#endif
int main(void) { makecontext(0, 0, 0); return 0; }]], {configvar = false})
	end
	if is_plat("linux") then
		probe_compiles("keyring", [[
#include <errno.h>
#include <asm/unistd.h>
#include <linux/keyctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void) {
    return syscall(__NR_keyctl, KEYCTL_READ, 0, NULL, NULL, 0);
}]], {configvar = false})
	end
	if not is_plat("macosx") then
		probe_compiles("tsa_cleanup", [[
struct __attribute__((capability("mutex"))) mutex {};
void lock(struct mutex *m) __attribute__((acquire_capability(m)));
void unlock(struct mutex *m) __attribute__((release_capability(m)));

void test(void) {
    struct mutex __attribute__((cleanup(unlock))) m;
    lock(&m);
}]], {configvar = false, cflags = {"-Wthread-safety", "-Werror"}})
	end
target_end()
