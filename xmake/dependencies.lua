add_requires("pkgconfig::glib-2.0 >=2.66.0", {alias = "glib", external = true})
add_requires("pkgconfig::zlib", {alias = "zlib"})
add_requires("pkgconfig::nettle >=3.10", {alias = "nettle"})
add_requires("pkgconfig::libtasn1", {alias = "tasn1"})
add_requires("pkgconfig::liblz4", {alias = "lz4"})
add_requires("pkgconfig::gmp", {alias = "gmp", optional = true})
add_requires("pkgconfig::hogweed >=3.4", {alias = "hogweed", optional = true})
add_requires("mlib", "keycodemapdb", "libyuv")

if is_plat("linux") then
	add_requires("pkgconfig::avahi-client", {alias = "avahi", optional = true})
	add_requires("pkgconfig::avahi-glib", {alias = "avahi_glib", optional = true})
end

if has_config("tcg_interpreter") then
	add_requires("pkgconfig::libffi >=3.0", {alias = "libffi"})
end

inferno_trace_backends = (get_config("trace_backends") or "nop"):split(",")
if table.contains(inferno_trace_backends, "ust") then
	add_requires("pkgconfig::lttng-ust >=2.1", {alias = "ust"})
end

option("__stap")
	set_showmenu(false)
	on_check(function (option)
		import("lib.detect.find_program")
		option:enable(find_program("stap") ~= nil)
	end)
option_end()
inferno_stap = table.contains(inferno_trace_backends, "dtrace") and has_config("__stap")

local function pkgconfig_feature(name, spec, description, plat)
	option(name)
		set_default(true)
		set_showmenu(true)
		set_category("Features")
		set_description(description)
		if plat then
			after_check(inferno_restrict_host(plat))
		end
	option_end()
	if has_config(name) then
		add_requires("pkgconfig::" .. spec, {alias = name, optional = true})
	end
end

pkgconfig_feature("gnutls", "gnutls >=3.5.18", "TLS support via GnuTLS")
pkgconfig_feature("zstd", "libzstd >=1.4.0", "zstd cluster compression for qcow2")
pkgconfig_feature("curl", "libcurl >=7.29.0", "curl block driver")
pkgconfig_feature("libssh", "libssh >=0.8.7", "SSH block driver")
pkgconfig_feature("libnfs", "libnfs >=1.9.3 <6.0.0", "NFS block driver")
pkgconfig_feature("glusterfs", "glusterfs-api >=3", "GlusterFS block driver")
pkgconfig_feature("blkio", "blkio", "libblkio block driver")
pkgconfig_feature("fuse", "fuse3 >=3.1", "FUSE block exports")
pkgconfig_feature("linux_io_uring", "liburing >=0.3", "io_uring AIO", "linux")
pkgconfig_feature("libusb", "libusb-1.0 >=1.0.13", "USB passthrough via libusb")
pkgconfig_feature("usb_redir", "libusbredirparser-0.5 >=0.6", "USB network redirection")
pkgconfig_feature("png", "libpng >=1.6.34", "PNG screendumps and VNC encoding")
pkgconfig_feature("vnc_jpeg", "libjpeg", "JPEG lossy compression for VNC")
pkgconfig_feature("slirp", "slirp", "User-mode networking via libslirp")
pkgconfig_feature("curses", "ncursesw", "curses user interface")
pkgconfig_feature("xkbcommon", "xkbcommon", "Generate the VNC and curses keymaps with inferno-keymap")
pkgconfig_feature("seccomp", "libseccomp >=2.3.0", "seccomp sandboxing")
pkgconfig_feature("libpmem", "libpmem", "Persistent memory support via libpmem")
pkgconfig_feature("libdaxctl", "libdaxctl >=57", "Device DAX support via libdaxctl")
pkgconfig_feature("jack", "jack", "JACK audio backend")
pkgconfig_feature("pipewire", "libpipewire-0.3 >=0.3.60", "PipeWire audio backend", "linux")
pkgconfig_feature("sndio", "sndio", "sndio audio backend")
pkgconfig_feature("pa", "libpulse", "PulseAudio audio backend", "linux")
pkgconfig_feature("alsa", "alsa", "ALSA audio backend", "linux")

option("vnc")
	set_default(true)
	set_showmenu(true)
	set_category("Features")
	set_description("VNC server")
option_end()

option("wxwidgets")
	set_default(true)
	set_showmenu(true)
	set_category("Features")
	set_description("wxWidgets user interface")
option_end()

option("wxconfig")
	set_showmenu(true)
	set_description("Path to the wx-config script")
option_end()

package("wxwidgets")
	set_kind("library")
	set_description("wxWidgets resolved through wx-config")

	on_fetch(function (package)
		import("core.project.config")
		import("lib.detect.find_program")
		local wxconfig = config.get("wxconfig") or "wx-config"
		local msys = os.host() == "windows"
		local function wxconfig_run(args)
			if msys then
				return os.iorunv("sh", table.join({"-c", '"$0" "$@"', wxconfig}, args))
			end
			return os.iorunv(wxconfig, args)
		end
		local function nativepath(path)
			if msys and path:startswith("/") then
				return os.iorunv("cygpath", {"-m", path}):trim()
			end
			return path
		end
		if not msys then
			wxconfig = find_program(wxconfig)
			if not wxconfig then
				return
			end
		end
		local cxxflags = try { function () return wxconfig_run({"--cxxflags"}) end }
		local libs	 = try { function () return wxconfig_run({"--libs", "core", "base"}) end }
		if not cxxflags or not libs then
			return
		end

		local toolkit = try { function () return wxconfig_run({"--query-toolkit"}):trim() end }
		local gtk = ({gtk2 = "gtk+-2.0", gtk3 = "gtk+-3.0"})[toolkit]
		if gtk then
			cxxflags = cxxflags .. " " .. os.iorunv("pkg-config", {"--cflags", gtk})
			libs = libs .. " " .. os.iorunv("pkg-config", {"--libs", gtk})
		end

		local info = {
			includedirs = {},
			defines	 = {},
			cxxflags	= {},
			linkdirs	= {},
			links	   = {},
			frameworks  = {},
			ldflags	 = {}
		}

		for _, flag in ipairs(os.argv(cxxflags:trim())) do
			if flag:startswith("-I") then
				table.insert(info.includedirs, nativepath(flag:sub(3)))
			elseif flag:startswith("-D") then
				table.insert(info.defines, flag:sub(3))
			elseif not flag:startswith("-std=") then
				table.insert(info.cxxflags, flag)
			end
		end

		local args = os.argv(libs:trim())
		local index = 1
		while index <= #args do
			local flag = args[index]
			if flag == "-framework" then
				table.insert(info.frameworks, args[index + 1])
				index = index + 1
			elseif flag:startswith("-L") then
				table.insert(info.linkdirs, nativepath(flag:sub(3)))
			elseif flag:startswith("-l") then
				table.insert(info.links, flag:sub(3))
			else
				table.insert(info.ldflags, flag)
			end
			index = index + 1
		end
		return info
	end)

	on_install(function (package)
		raise("wx-config not found; set it with: xmake f --wxconfig=<path> or disable wxWidgets (--wxwidgets=n)")
	end)
package_end()

if has_config("wxwidgets") then
	add_requires("wxwidgets")
end

option("coreaudio")
	set_default(true)
	set_showmenu(true)
	set_category("Features")
	set_description("CoreAudio audio backend (macOS)")
	after_check(inferno_restrict_host("macosx"))
option_end()

option("vmnet")
	set_showmenu(true)
	set_category("Features")
	set_description("vmnet.framework networking (macOS)")
	add_cincludes("vmnet/vmnet.h")
	add_csnippets("vmnet", "int main(void) { return VMNET_BRIDGED_MODE; }")
	add_frameworks("vmnet")
	after_check(inferno_restrict_host("macosx"))
option_end()

option("vnc_sasl")
	set_showmenu(true)
	set_category("Features")
	set_description("SASL authentication for VNC")
	add_deps("vnc")
	add_links("sasl2")
	add_cincludes("sasl/sasl.h")
	after_check(function (option)
		if not option:dep("vnc"):enabled() then
			option:enable(false, {force = true})
		end
	end)
option_end()

option("vde")
	set_showmenu(true)
	set_category("Features")
	set_description("VDE network backend")
	add_links("vdeplug")
	add_cincludes("libvdeplug.h")
	add_csnippets("vde", [[
int main(void)
{
    struct vde_open_args a = {0, 0, 0};
    char s[] = "";
    vde_open(s, s, &a);
    return 0;
}]])
option_end()

option("cap_ng")
	set_showmenu(true)
	set_category("Features")
	set_description("libcap-ng support for inferno-bridge-helper")
	add_links("cap-ng")
	add_cincludes("cap-ng.h")
	add_csnippets("cap_ng", [[
int main(void)
{
    capng_capability_to_name(CAPNG_EFFECTIVE);
    return 0;
}]])
option_end()

option("auth_pam")
	set_showmenu(true)
	set_category("Features")
	set_description("PAM access control")
	add_links("pam")
	add_cincludes("stddef.h", "security/pam_appl.h")
	add_csnippets("auth_pam", [[
int main(void)
{
    const char *service_name = "qemu";
    const char *user = "frank";
    const struct pam_conv pam_conv = { 0 };
    pam_handle_t *pamh = NULL;
    pam_start(service_name, user, &pam_conv, &pamh);
    return 0;
}]])
option_end()

option("bzip2")
	set_showmenu(true)
	set_category("Features")
	set_description("bzip2 support for DMG images")
	add_links("bz2")
	add_cincludes("bzlib.h")
	add_csnippets("bzip2", "int main(void) { BZ2_bzlibVersion(); return 0; }")
option_end()

option("linux_aio")
	set_showmenu(true)
	set_category("Features")
	set_description("Linux AIO support")
	add_links("aio")
	add_cincludes("libaio.h")
option_end()

option("rbd")
	set_showmenu(true)
	set_category("Features")
	set_description("Ceph RBD block driver")
	add_links("rbd", "rados")
	add_cincludes("stdio.h", "rbd/librbd.h")
	add_csnippets("rbd", [[
int main(void) {
    rados_t cluster;
    rados_create(&cluster, NULL);
#if LIBRBD_VERSION_CODE < LIBRBD_VERSION(1, 12, 0)
#error
#endif
    return 0;
}]])
option_end()

option("oss")
	set_showmenu(true)
	set_category("Features")
	set_description("OSS audio backend")
	add_deps("__netbsd")
	add_cincludes("sys/soundcard.h")
	after_check(function (option)
		if option:enabled() and option:dep("__netbsd"):enabled() then
			option:add("links", "ossaudio")
		end
	end)
option_end()

option("iconv")
	set_showmenu(false)
	on_check(function (option)
		import("lib.detect.check_csnippets")
		local snippet = [[
#include <iconv.h>
int main(void) {
    iconv_t conv = iconv_open("WCHAR_T", "UCS-2");
    return conv != (iconv_t) -1;
}]]
		if check_csnippets(snippet, {configs = {links = "iconv"}}) then
			option:add("links", "iconv")
			option:enable(true)
		else
			option:enable(check_csnippets(snippet))
		end
	end)
option_end()

option("libutil")
	set_showmenu(false)
	add_links("util")
option_end()

option("libm")
	set_showmenu(false)
	add_links("m")
option_end()

option("librt")
	set_showmenu(false)
	add_links("rt")
option_end()

option("libsocket")
	set_showmenu(false)
	if is_plat("windows", "mingw") then
		add_syslinks("ws2_32")
	elseif is_plat("haiku") then
		add_links("posix_error_mapper", "network", "bsd")
	end
	after_check(function (option)
		option:enable(is_plat("windows", "mingw", "haiku"), {force = true})
	end)
option_end()

option("libinotify")
	set_showmenu(false)
	add_links("inotify")
option_end()

option("dsound")
	set_showmenu(true)
	set_category("Features")
	set_description("DirectSound audio backend (Windows)")
	add_cincludes("dsound.h")
	add_syslinks("ole32", "dxguid")
	after_check(inferno_restrict_host("windows") or inferno_restrict_host("mingw"))
option_end()

option("dnsapi")
	set_showmenu(false)
	add_cincludes("windows.h", "windns.h")
	after_check(inferno_restrict_host("windows") or inferno_restrict_host("mingw"))
option_end()

for flavour, macro in pairs({freebsd = "__FreeBSD__", netbsd = "__NetBSD__", openbsd = "__OpenBSD__"}) do
	option("__" .. flavour)
		set_showmenu(false)
		add_csnippets(flavour, ("#ifndef %s\n#error\n#endif\nint main(void) { return 0; }"):format(macro))
	option_end()
end

option("lzfse")
	set_showmenu(false)
	add_links("lzfse")
	add_cincludes("lzfse.h")
	add_csnippets("lzfse", "int main(void) { lzfse_decode_scratch_size(); return 0; }")
	local prefixes = is_plat("macosx") and {is_arch("arm64") and "/opt/homebrew" or "/usr/local", "/opt/local"} or {"/usr/local"}
	for _, prefix in ipairs(prefixes) do
		if os.isfile(path.join(prefix, "include", "lzfse.h")) then
			add_includedirs(prefix .. "/include")
			add_linkdirs(prefix .. "/lib")
			break
		end
	end
	after_check(function (option)
		if not option:enabled() then
			raise("liblzfse is required")
		end
	end)
option_end()

option("__io_prep_pwritev2")
	set_showmenu(false)
	add_deps("linux_aio")
	add_cincludes("libaio.h")
	add_csnippets("io_prep_pwritev2", "void f(void) { (void) io_prep_pwritev2; }")
	set_configvar("HAVE_IO_PREP_PWRITEV2", "", {quote = false})
	after_check(function (option)
		if not option:dep("linux_aio"):enabled() then
			option:enable(false, {force = true})
		end
	end)
option_end()

option("__rbd_namespace_exists")
	set_showmenu(false)
	add_deps("rbd")
	add_links("rbd", "rados")
	add_cfuncs("rbd_namespace_exists")
	add_cincludes("rbd/librbd.h")
	set_configvar("HAVE_RBD_NAMESPACE_EXISTS", "", {quote = false})
	after_check(function (option)
		if not option:dep("rbd"):enabled() then
			option:enable(false, {force = true})
		end
	end)
option_end()

local function package_check(name, spec, configvar, includes, snippet)
	option(name)
		set_showmenu(false)
		if configvar then
			set_configvar(configvar, "", {quote = false})
		end
		on_check(function (option)
			import("lib.detect.find_package")
			import("lib.detect.check_csnippets")
			local pkg = find_package("pkgconfig::" .. spec)
			option:enable(pkg ~= nil and check_csnippets(snippet, {includes = includes, configs = pkg}))
		end)
	option_end()
end

package_check("__nettle_xts", "nettle", nil, "nettle/xts.h", "")

if has_config("seccomp") then
	package_check("__seccomp_sysrawrc", "libseccomp", "CONFIG_SECCOMP_SYSRAWRC", "seccomp.h",
		"void f(void) { (void) SCMP_FLTATR_API_SYSRAWRC; }")
end

if has_config("linux_io_uring") then
	package_check("__liburing_usable", "liburing", nil, {"liburing.h", "linux/errqueue.h"}, "")
	package_check("__io_uring_prep_writev2", "liburing", "HAVE_IO_URING_PREP_WRITEV2", "liburing.h",
		"void f(void) { (void) io_uring_prep_writev2; }")
end

inferno_have_io_uring = has_package("linux_io_uring") and has_config("__liburing_usable")
inferno_have_curses = has_package("curses") and has_config("iconv")
inferno_have_vnc = has_config("vnc")
inferno_have_hogweed = has_package("gmp") and has_package("hogweed")
