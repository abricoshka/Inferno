set_project("Inferno")
set_xmakever("3.0.0")
set_languages("gnu11")
set_warnings("all", "extra")
set_policy("build.progress_style", "multirow")
add_rules("inferno.dependir")
set_allowedmodes("debug", "releasedbg", "release")
set_defaultmode("releasedbg")
add_moduledirs("xmake/modules")

inferno_cpu = ({
	i386 = "x86", x86 = "x86",
	x86_64 = "x86_64", x64 = "x86_64",
	arm64 = "aarch64", ["arm64-v8a"] = "aarch64", aarch64 = "aarch64",
	arm = "arm", armv7 = "arm", armv7s = "arm", ["armv7-a"] = "arm", armeabi = "arm", ["armeabi-v7a"] = "arm",
	loong64 = "loongarch64", loongarch64 = "loongarch64",
	mips = "mips", mipsel = "mips", mips64 = "mips64", mips64el = "mips64",
	ppc = "ppc", ppc64 = "ppc64", ppc64el = "ppc64", ppc64le = "ppc64",
	riscv32 = "riscv32", riscv64 = "riscv64",
	s390x = "s390x", sparc64 = "sparc64", wasm32 = "wasm32",
})[get_config("arch") or os.arch()] or "unknown"
inferno_host_arch = ({x86 = "i386", mips64 = "mips", riscv32 = "riscv", riscv64 = "riscv"})[inferno_cpu] or inferno_cpu
if has_config("tcg_interpreter") then
	inferno_tcg_arch = "tci"
else
	inferno_tcg_arch = ({x86_64 = "i386", ppc64 = "ppc"})[inferno_host_arch] or inferno_host_arch
end
inferno_linux_arch = ({
	aarch64 = "arm64", loongarch64 = "loongarch", mips = "mips", mips64 = "mips", ppc = "powerpc", ppc64 = "powerpc",
	riscv32 = "riscv", riscv64 = "riscv", s390x = "s390", x86 = "x86", x86_64 = "x86",
})[inferno_cpu]

inferno_target = "aarch64-softmmu"
inferno_gendir = "$(builddir)/.gens/inferno"
inferno_subdirs = {"accel", "audio", "authz", "backends", "block", "chardev", "crypto", "fpu", "fspatcher", "gdbstub",
	"hw", "io", "monitor", "net", "qapi", "qobject", "qom", "stats", "stubs", "system", "target", "tcg", "trace", "ui",
	"util"}

includes("@builtin/check")
includes("xmake/packages.lua")
includes("xmake/options.lua")
includes("xmake/dependencies.lua")
includes("xmake/devices.lua")
includes("xmake/rules/*.lua")

if is_plat("mingw") then
	inferno_coroutine_backend = "windows"
elseif is_plat("wasm") then
	inferno_coroutine_backend = "wasm"
elseif get_config("coroutine_backend") and get_config("coroutine_backend") ~= "auto" then
	inferno_coroutine_backend = get_config("coroutine_backend")
elseif not is_plat("macosx") and has_config("__ucontext") then
	inferno_coroutine_backend = "ucontext"
else
	inferno_coroutine_backend = "sigaltstack"
end

inferno_isa_flags = {}
if inferno_cpu == "x86" or inferno_cpu == "x86_64" then
	local level = tonumber(get_config("x86_version") or "1")
	if inferno_cpu == "x86" and not has_config("__i386_sync") then
		table.insert(inferno_isa_flags, "-march=i486")
	end
	if level >= 1 then
		table.insert(inferno_isa_flags, inferno_cpu == "x86" and "-mfpmath=sse" or "-mcx16")
	end
	if level >= 2 then
		table.insert(inferno_isa_flags, "-mpopcnt")
	end
	if level >= 3 then
		table.join2(inferno_isa_flags, {"-mmovbe", "-mabm", "-mbmi", "-mbmi2", "-mfma", "-mf16c"})
	end
	table.join2(inferno_isa_flags, ({{"-msse2"}, {"-msse4.2"}, {"-mavx2"},
		{"-mavx512f", "-mavx512bw", "-mavx512cd", "-mavx512dq", "-mavx512vl"}})[level] or {})
	add_cxflags(inferno_isa_flags, {force = true})
	add_mxflags(inferno_isa_flags, {force = true})
	if level >= 2 then
		add_cxflags("-mneeded", {tools = "gcc"})
	end
end

add_defines("_GNU_SOURCE", "_FILE_OFFSET_BITS=64", "_LARGEFILE_SOURCE")
add_cxflags("-fno-strict-aliasing", "-fno-common", "-fwrapv", {force = true})
add_mxflags("-fno-strict-aliasing", "-fno-common", "-fwrapv", "-fno-objc-arc", {force = true})
if is_mode("debug") then
	set_symbols("debug")
	set_optimize("none")
elseif is_mode("releasedbg") then
	set_symbols("debug")
	set_optimize("faster")
else
	set_optimize("fastest")
	add_defines("NDEBUG")
end
if is_mode("debug", "releasedbg") then
	add_cxflags("-fno-omit-frame-pointer", {force = true})
	add_mxflags("-fno-omit-frame-pointer", {force = true})
else
	add_defines("G_DISABLE_ASSERT")
end
if is_plat("macosx") then
	add_cflags("-DOS_OBJECT_USE_OBJC=0", {tools = "gcc"})
elseif is_plat("haiku") then
	add_defines("B_USE_POSITIVE_POSIX_ERRORS", "_BSD_SOURCE")
	add_cxflags("-fPIC", {force = true})
elseif is_plat("wasm") then
	add_cxflags("-pthread", {force = true})
	add_ldflags("-pthread", "-sASYNCIFY=1", "-sPROXY_TO_PTHREAD=1", "-sFORCE_FILESYSTEM", "-sALLOW_TABLE_GROWTH",
		"-sTOTAL_MEMORY=2GB", "-sWASM_BIGINT", "-sEXPORT_ES6=1", "-sASYNCIFY_IMPORTS=ffi_call_js",
		"-sEXPORTED_RUNTIME_METHODS=addFunction,removeFunction,TTY,FS", {force = true})
end
add_cxflags("-ftrivial-auto-var-init=zero", "-fstack-protector-strong")
add_mxflags("-ftrivial-auto-var-init=zero", "-fstack-protector-strong")
add_ldflags("-fstack-protector-strong")
if not has_config("__openbsd") then
	add_cxflags("-fzero-call-used-regs=used-gpr")
	add_mxflags("-fzero-call-used-regs=used-gpr")
end
if is_plat("linux", "bsd", "haiku") then
	add_ldflags("-Wl,-z,relro", "-Wl,-z,now")
elseif is_plat("mingw") then
	add_ldflags("-Wl,--no-seh", "-Wl,--nxcompat", "-Wl,--dynamicbase", "-Wl,--high-entropy-va")
end

inferno_warnings = {
	"-Wempty-body",
	"-Wendif-labels",
	"-Wexpansion-to-defined",
	"-Wformat-security",
	"-Wformat-y2k",
	"-Wignored-qualifiers",
	"-Winit-self",
	"-Wmissing-format-attribute",
	"-Wmissing-prototypes",
	"-Wnested-externs",
	"-Wold-style-definition",
	"-Wredundant-decls",
	"-Wstrict-prototypes",
	"-Wtype-limits",
	"-Wundef",
	"-Wno-vla",
	"-Wwrite-strings",
	"-Wno-gnu-variable-sized-type-not-at-end",
	"-Wno-initializer-overrides",
	"-Wno-missing-include-dirs",
	"-Wno-psabi",
	"-Wno-shift-negative-value",
	"-Wno-string-plus-int",
	"-Wno-tautological-type-limit-compare",
	"-Wno-typedef-redefinition",
}
add_cflags(table.unpack(inferno_warnings))
add_mflags(inferno_warnings, {force = true})
add_cflags("-Wimplicit-fallthrough=2", "-Wold-style-declaration", "-Wshadow=local", {tools = "gcc"})
if has_config("__tsa_cleanup") then
	add_cflags("-Wthread-safety")
end

local quote_dirs = {".", "include"}
if os.isdir(path.join(os.projectdir(), "host", "include", inferno_host_arch)) then
	table.insert(quote_dirs, "host/include/" .. inferno_host_arch)
end
table.join2(quote_dirs, {"host/include/generic", "tcg/" .. inferno_tcg_arch})
for _, dir in ipairs(quote_dirs) do
	add_cxflags("-iquote" .. path.join(os.projectdir(), dir), {force = true})
	add_mxflags("-iquote" .. path.join(os.projectdir(), dir), {force = true})
end
if is_plat("linux") then
	add_sysincludedirs("linux-headers")
end

includes("xmake/genh.lua")

target("qemuutil")
	set_kind("static")
	on_install(function (target) end)
	add_deps("genh", "event-loop-base")
	add_rules("inferno.genh_sources")
	add_packages("glib", "gnutls", "linux_io_uring", "ust", {public = true})
	add_syslinks("pthread", {public = true})
	add_options("libm", "librt", "libsocket")
	if is_plat("mingw") then
		add_syslinks("winmm", "pathcch", "synchronization", {public = true})
	elseif is_plat("bsd") then
		add_options("libutil", "libinotify")
	end
target_end()

target("qom")
	set_kind("object")
	add_deps("genh")
	add_packages("glib")
target_end()

target("event-loop-base")
	set_kind("object")
	add_deps("genh", "qom")
	add_packages("glib")
	add_files("event-loop-base.c")
target_end()

target("authz")
	set_kind("object")
	add_deps("genh", "qom")
	add_packages("glib")
	add_options("auth_pam")
target_end()

target("crypto")
	set_kind("object")
	add_deps("genh", "authz", "qom")
	add_packages("glib", "gnutls", "nettle", "gmp", "hogweed", {public = true})
target_end()

target("io")
	set_kind("object")
	add_deps("genh", "crypto", "qom")
	add_packages("glib")
target_end()

target("block")
	set_kind("object")
	add_deps("genh", "crypto", "io")
	add_packages("glib", "zlib", "zstd", "linux_io_uring", "blkio", "curl", "glusterfs", "libnfs", "libssh", {public = true})
	add_options("lzfse", "linux_aio", "bzip2", "rbd")
target_end()

target("blockdev")
	set_kind("object")
	add_deps("genh", "block", "event-loop-base")
	add_packages("glib", "fuse", {public = true})
target_end()

target("qmp")
	set_kind("object")
	add_deps("genh")
	add_packages("glib")
target_end()

target("chardev")
	set_kind("object")
	add_deps("genh", "io")
	add_packages("glib")
	add_options("libutil")
target_end()

target("hwcore")
	set_kind("object")
	add_deps("genh")
	add_packages("glib")
target_end()

target("system")
	set_kind("object")
	add_deps("genh", "authz", "blockdev", "chardev", "crypto", "io", "qmp", "qom", "qemuutil")
	add_defines("CONFIG_SOFTMMU")
	add_packages("glib", "pixman", "zlib", "png", "vnc_jpeg", "slirp", "sdl", "sdl_image", "gtk", "gtkx11", "vte", "x11",
		"curses", "libusb", "usb_redir", "libpmem", "libdaxctl", "seccomp", "lz4", "avahi", "avahi_glib",
		"jack", "pipewire", "sndio", "pa", "alsa", "libffi", "libyuv", {public = true})
	add_options("vnc_sasl", "vde", "iconv", "oss", "dsound", "dnsapi")
	if has_config("vnc_sasl") then
		add_defines("STRUCT_IOVEC_DEFINED", {public = true})
	end
target_end()

target("common")
	set_kind("object")
	add_deps("genh", "hwcore", "qom", "qemuutil")
	add_packages("glib")
target_end()

target("common_arm")
	set_kind("object")
	add_deps("genh", "common", "system")
	add_includedirs("target/arm")
	add_defines("CPU_DEFS_H")
target_end()

target("system_arm")
	set_kind("object")
	add_deps("genh", "common", "system")
	add_includedirs("target/arm")
	add_defines("CPU_DEFS_H", "CONFIG_SOFTMMU")
	add_packages("tasn1", {public = true})
	add_options("lzfse")
target_end()

target("qemu-" .. inferno_target)
	set_kind("object")
	add_deps("genh", "common", "common_arm", "system", "system_arm")
	add_rules("inferno.decodetree", "inferno.gdbstub_xml")
	add_includedirs("target/arm")
	add_defines("COMPILING_PER_TARGET",
		'CONFIG_TARGET="' .. inferno_target .. '-config-target.h"',
		'CONFIG_DEVICES="' .. inferno_target .. '-config-devices.h"')
target_end()

inferno_keymaps = {
	["ar"] = "-l ara",
	["bepo"] = "-l fr -v dvorak",
	["cz"] = "-l cz",
	["da"] = "-l dk",
	["de"] = "-l de -v nodeadkeys",
	["de-ch"] = "-l ch",
	["en-gb"] = "-l gb",
	["en-us"] = "-l us",
	["es"] = "-l es",
	["et"] = "-l et",
	["fi"] = "-l fi",
	["fo"] = "-l fo",
	["fr"] = "-l fr -v nodeadkeys",
	["fr-be"] = "-l be",
	["fr-ca"] = "-l ca -v fr",
	["fr-ch"] = "-l ch -v fr",
	["hr"] = "-l hr",
	["hu"] = "-l hu",
	["is"] = "-l is",
	["it"] = "-l it",
	["ja"] = "-l jp -m jp106",
	["lt"] = "-l lt",
	["lv"] = "-l lv",
	["mk"] = "-l mk",
	["nl"] = "-l nl",
	["no"] = "-l no",
	["pl"] = "-l pl",
	["pt"] = "-l pt",
	["pt-br"] = "-l br",
	["ru"] = "-l ru",
	["th"] = "-l th",
	["tr"] = "-l tr",
}

if has_package("xkbcommon") then
	target("inferno-keymap")
		set_kind("binary")
		add_deps("genh", "qemuutil")
		add_rules("inferno.keymaps")
		add_files("qemu-keymap.c", "ui/input-keymap.c")
		add_packages("xkbcommon")
		if is_plat("mingw") then
			add_files("version.rc")
		end
		for name, args in table.orderpairs(inferno_keymaps) do
			add_values("inferno.keymaps", name .. "=" .. args)
		end
		if not has_config("tools") then
			on_install(function (target) end)
		end
	target_end()
end

local function tapsets(name)
	if inferno_stap then
		for _, suffix in ipairs({".stp", "-simpletrace.stp", "-log.stp"}) do
			add_installfiles(path.join(inferno_gendir, "systemtap", name .. suffix),
				{prefixdir = "share/systemtap/tapset", always_added = true})
		end
	end
end

local function emulator(name, gui)
	target(name)
		set_kind("binary")
		add_deps("genh", "qemu-" .. inferno_target)
		add_files("system/main.c")
		add_includedirs("target/arm")
		add_defines("COMPILING_PER_TARGET",
			'CONFIG_TARGET="' .. inferno_target .. '-config-target.h"',
			'CONFIG_DEVICES="' .. inferno_target .. '-config-devices.h"')
		if is_plat("macosx") then
			add_rules("inferno.entitlement")
			add_frameworks("CoreFoundation", "IOKit")
			if has_config("hvf") then
				add_frameworks("Hypervisor")
			end
			if has_config("cocoa") then
				add_frameworks("Cocoa", "CoreVideo", "QuartzCore")
			end
			if has_config("coreaudio") then
				add_frameworks("CoreAudio")
			end
			if has_config("vmnet") then
				add_frameworks("vmnet")
			end
		elseif is_plat("mingw") then
			add_files("version.rc")
			if gui then
				add_ldflags("-mwindows", {force = true})
			end
		end
		if has_config("tcg") and has_config("__openbsd") then
			add_ldflags("-Wl,-z,wxneeded")
		end
		if not gui then
			add_rules("inferno.bundle")
			if has_package("xkbcommon") then
				add_deps("inferno-keymap")
				for keymap in pairs(inferno_keymaps) do
					add_installfiles(path.join(inferno_gendir, "keymaps", keymap), {prefixdir = "share/qemu/keymaps",
						always_added = true})
				end
			end
			add_installfiles("ui/qemu.desktop", {prefixdir = "share/applications"})
			for _, size in ipairs({16, 24, 32, 48, 64, 128, 256, 512}) do
				local dim = size .. "x" .. size
				add_installfiles("ui/icons/qemu_" .. dim .. ".png", {prefixdir = "share/icons/hicolor/" .. dim .. "/apps",
					filename = "qemu.png"})
			end
			add_installfiles("ui/icons/qemu_32x32.bmp", {prefixdir = "share/icons/hicolor/32x32/apps", filename = "qemu.bmp"})
			add_installfiles("ui/icons/qemu.svg", {prefixdir = "share/icons/hicolor/scalable/apps"})
			add_installfiles("ui/icons/CKQEMUBootSplash_512x512@2x.png", {prefixdir = "share/icons/hicolor/512x512/apps",
				filename = "CKQEMUBootSplash@2x.png"})
			if not (#inferno_trace_backends == 1 and inferno_trace_backends[1] == "nop") then
				add_installfiles(path.join(inferno_gendir, "trace-events-all"), {prefixdir = "share/qemu", always_added = true})
			end
			tapsets(name)
			if inferno_stap then
				add_installfiles("scripts/inferno-trace-stap", {prefixdir = "bin"})
			end
		end
	target_end()
end

emulator("inferno-aarch64")
if is_plat("mingw") and (has_package("sdl") or has_package("gtk")) then
	emulator("inferno-aarch64w", true)
end

if has_config("tools") then
	target("inferno-img")
		set_kind("binary")
		add_deps("genh", "authz", "block", "crypto", "io", "qom", "qemuutil")
		add_files("qemu-img.c")
		tapsets("inferno-img")
		if is_plat("macosx") then
			add_frameworks("CoreFoundation", "IOKit")
		elseif is_plat("mingw") then
			add_files("version.rc")
		end
	target_end()

	target("inferno-edid")
		set_kind("binary")
		add_deps("genh", "qemuutil")
		add_files("qemu-edid.c", "hw/display/edid-generate.c")
		if is_plat("mingw") then
			add_files("version.rc")
		end
	target_end()

	if is_plat("linux") then
		target("inferno-bridge-helper")
			set_kind("binary")
			add_deps("genh", "qemuutil")
			add_files("qemu-bridge-helper.c")
			add_options("cap_ng")
			set_prefixdir("/", {bindir = "libexec"})
		target_end()
	end

end

includes(table.unpack(inferno_subdirs))

target("block")
	add_files("block.c", "blockjob.c", "job.c")
target_end()

target("blockdev")
	add_files("blockdev.c", "iothread.c", "job-qmp.c")
	if is_plat("wasm") then
		add_files("os-wasm.c")
	elseif not is_plat("mingw") then
		add_files("os-posix.c")
	end
target_end()

target("common")
	add_files("cpu-common.c", "target-info.c", "page-vary-common.c")
target_end()

target("system")
	add_files("cpu-target.c")
	if is_plat("mingw") then
		add_files("os-win32.c")
	end
target_end()

target("qemu-" .. inferno_target)
	add_files("page-vary-target.c", "target-info-stub.c")
target_end()
