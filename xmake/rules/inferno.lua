rule("inferno.genh")
	on_config(function (target)
		import("core.base.option")
		import("core.base.semver")
		import("core.project.config")
		import("inferno.codegen")
		import("inferno.genh")

		local function define(name, enabled)
			if enabled then
				target:set("configvar", name, "", {quote = false})
			end
		end
		local function version_at_least(pkgname, version)
			local pkg = target:pkg(pkgname)
			return pkg ~= nil and pkg:get("version") ~= nil and semver.compare(pkg:get("version"), version) >= 0
		end

		local version = genh.version()
		target:set("configvar", "QEMU_VERSION", version)
		target:set("configvar", "QEMU_VERSION_MAJOR", 10) -- for compat
		target:set("configvar", "QEMU_VERSION_MINOR", 2)
		target:set("configvar", "QEMU_VERSION_MICRO", 2)

		local prefix = path.unix(config.get("prefix"))
		local sysconfdir = prefix == "/usr" and "/etc" or prefix .. "/etc"
		local localstatedir = prefix == "/usr" and "/var" or prefix .. "/var"
		target:set("configvar", "CONFIG_PREFIX", prefix)
		target:set("configvar", "CONFIG_BINDIR", prefix .. "/bin")
		target:set("configvar", "CONFIG_SYSCONFDIR", sysconfdir)
		target:set("configvar", "CONFIG_QEMU_CONFDIR", sysconfdir .. "/inferno")
		target:set("configvar", "CONFIG_QEMU_DATADIR", prefix .. "/share/inferno")
		target:set("configvar", "CONFIG_QEMU_ICONDIR", prefix .. "/share/icons")
		target:set("configvar", "CONFIG_QEMU_HELPERDIR", prefix .. "/libexec")
		target:set("configvar", "CONFIG_QEMU_LOCALSTATEDIR", localstatedir)

		define("HAVE_GLIB_WITH_SLICE_ALLOCATOR", not version_at_least("glib", "2.75.3"))
		define("CONFIG_FUSE_LSEEK", version_at_least("fuse", "3.8"))
		define("CONFIG_BLKIO_WRITE_EROS_FUA", version_at_least("blkio", "1.4.0"))
		define("CONFIG_GLUSTERFS_XLATOR_OPT", version_at_least("glusterfs", "4"))
		define("CONFIG_GLUSTERFS_DISCARD", version_at_least("glusterfs", "5"))
		for _, name in ipairs({"FALLOCATE", "ZEROFILL", "FTRUNCATE_HAS_STAT", "IOCB_HAS_STAT"}) do
			define("CONFIG_GLUSTERFS_" .. name, version_at_least("glusterfs", "6"))
		end
		define("CONFIG_TSAN", target:policy("build.sanitizer.thread"))

		local cpu = target:values("inferno.cpu")
		if table.contains({"x86", "arm", "mips", "ppc", "riscv32", "wasm32"}, cpu) then
			raise("No accelerator available for target %s on a 32-bit host", target:values("inferno.target"))
		end
		if config.get("tcg") and not config.get("tcg_interpreter") then
			if cpu == "unknown" then
				raise("unsupported host CPU %s, try --tcg_interpreter=y", config.arch())
			elseif cpu == "wasm32" then
				raise("WebAssembly hosts require --tcg_interpreter=y")
			end
		end
		local coroutine_backend = target:values("inferno.coroutine_backend")
		local supported_backends = target:is_plat("windows", "mingw") and {"windows"} or target:is_plat("wasm") and {"wasm"} or
			table.join(config.get("__ucontext") and {"ucontext"} or {}, "sigaltstack")
		if not table.contains(supported_backends, coroutine_backend) then
			raise("coroutine backend %s is not available, available backends: %s", coroutine_backend,
				table.concat(supported_backends, ", "))
		end
		if cpu == "x86" or cpu == "x86_64" then
			local level = tonumber(config.get("x86_version"))
			if level == 0 and cpu == "x86_64" then
				raise("x86_64-v1 is required for x86-64 hosts")
			elseif level >= 3 and not (config.get("__CONFIG_CPUID_H") and config.get("__CONFIG_AVX2_OPT")) then
				raise("cannot enable AVX optimizations due to missing intrinsics")
			elseif level >= 4 and not config.get("__avx512bw") then
				raise("cannot enable AVX512 optimizations due to missing intrinsics")
			end
		end
		if target:is_plat("windows", "mingw") and cpu == "aarch64" and not config.get("__mingw_setjmp") then
			raise("mingw must provide setjmp/longjmp for windows-arm64")
		end

		local linux_arch = target:values("inferno.linux_arch")
		if target:is_plat("linux") and linux_arch then
			local headers = codegen.srcfile("linux-headers", "asm-" .. linux_arch)
			assert(os.isdir(headers), "linux-headers/asm-%s does not exist", linux_arch)
			local asm = path.join(codegen.gendir(), "linux-headers", "asm")
			os.tryrm(asm)
			os.mkdir(path.directory(asm))
			os.ln(headers, asm)
		end
	end)

	on_prepare(function (target, opt)
		import("inferno.genh")
		import("inferno.qapi")
		import("inferno.trace")
		qapi.generate(target, opt)
		trace.generate(target, opt)
		genh.generate(target, opt)
	end)
rule_end()

rule("inferno.genh_sources")
	after_load(function (target)
		import("inferno.qapi")
		import("inferno.trace")
		local genh = target:dep("genh")
		for _, file in ipairs(table.join(qapi.sources(genh), trace.sources(genh))) do
			target:add("files", file, {always_added = true})
		end
	end)
rule_end()

rule("inferno.decodetree")
	set_extensions(".decode")
	on_load(function (target)
		target:add("includedirs", path.join(target:autogendir(), "decodetree"))
	end)
	on_prepare_file(function (target, sourcefile, opt)
		import("inferno.codegen")
		local script = codegen.srcfile("scripts", "decodetree.py")
		local output = path.join(target:autogendir(), "decodetree", "decode-" .. path.basename(sourcefile) .. ".c.inc")
		local args = table.wrap(target:fileconfig(sourcefile).decodetree_args)
		codegen.generate(target, "decodetree", {progress = opt.progress, inputs = {sourcefile, script}, values = args, outputs = {output}}, function ()
			os.vrunv(codegen.python(), table.join(script, sourcefile, args, "-o", output))
		end)
	end)
rule_end()

rule("inferno.gdbstub_xml")
	set_extensions(".xml")
	on_load(function (target)
		target:add("files", path.join(target:autogendir(), "gdbstub", "gdbstub-xml.c"), {always_added = true})
	end)
	on_prepare_files(function (target, sourcebatch, opt)
		import("inferno.codegen")
		local script = codegen.srcfile("scripts", "feature_to_c.py")
		local output = path.join(target:autogendir(), "gdbstub", "gdbstub-xml.c")
		codegen.generate(target, "gdbstub_xml", {progress = opt.progress, inputs = table.join(sourcebatch.sourcefiles, script),
			values = sourcebatch.sourcefiles, outputs = {output}}, function ()
			codegen.capture(codegen.python(), table.join(script, sourcebatch.sourcefiles), output)
		end)
	end)
rule_end()

rule("inferno.block_gen")
	on_load(function (target)
		target:add("files", path.join(target:autogendir(), "block", "block-gen.c"), {always_added = true})
	end)
	on_prepare(function (target, opt)
		import("inferno.codegen")
		local script = codegen.srcfile("scripts", "block-coroutine-wrapper.py")
		local output = path.join(target:autogendir(), "block", "block-gen.c")
		local inputs = {}
		for _, header in ipairs(table.wrap(target:values("inferno.block_gen_headers"))) do
			table.insert(inputs, codegen.srcfile(header))
		end
		codegen.generate(target, "block_gen", {progress = opt.progress, inputs = table.join(inputs, script), outputs = {output}}, function ()
			os.vrunv(codegen.python(), table.join(script, output, inputs))
		end)
	end)
rule_end()

rule("inferno.bundle")
	after_build(function (target)
		import("core.project.config")
		local bundledir = path.join(target:targetdir(), "qemu-bundle", (config.get("prefix"):gsub("^/+", "")))
		local srcfiles, dstfiles = target:installfiles(bundledir)
		for index, srcfile in ipairs(srcfiles) do
			local dstfile = dstfiles[index]
			os.tryrm(dstfile)
			os.mkdir(path.directory(dstfile))
			if target:is_plat("windows", "mingw") then
				os.vcp(path.absolute(srcfile), dstfile)
			else
				os.ln(path.absolute(srcfile), dstfile)
			end
		end
	end)
rule_end()

rule("inferno.dlls")
	after_build(function (target)
		if target:is_plat("windows", "mingw") then
			import("inferno.dlls").bundle(target, target:targetdir())
		end
	end)
	after_install(function (target)
		if target:is_plat("windows", "mingw") then
			import("inferno.dlls").bundle(target, path.join(target:installdir(), "bin"))
		end
	end)
rule_end()

rule("inferno.entitlement")
	after_build(function (target)
		import("inferno.codegen")
		if target:is_plat("macosx") then
			codegen.entitle(target:targetfile())
		end
	end)
	after_install(function (target)
		import("inferno.codegen")
		if target:is_plat("macosx") then
			codegen.entitle(path.join(target:installdir(), "bin", target:filename()))
		end
	end)
rule_end()

rule("inferno.glib_slice")
	on_load(function (target)
		import("core.base.semver")
		local glib = target:pkg("glib")
		if glib and glib:get("version") and semver.compare(glib:get("version"), "2.75.3") < 0 then
			target:add("files", path.join(os.projectdir(), "util", "qtree.c"))
		end
	end)
rule_end()

rule("inferno.dependir")
	before_build(function (target)
		os.mkdir(target:dependir())
	end)
rule_end()

rule("inferno.keymaps")
	after_build(function (target)
		import("inferno.codegen")
		if target:is_cross() then
			wprint("cannot run %s when cross-compiling, keymaps were not generated", target:name())
			return
		end
		for _, keymap in ipairs(table.wrap(target:values("inferno.keymaps"))) do
			local name, args = keymap:match("^([^=]+)=(.*)$")
			local output = path.join(codegen.gendir(), "keymaps", name)
			codegen.generate(target, "keymap", {inputs = {target:targetfile()}, values = {args}, outputs = {output}}, function ()
				os.vrunv(target:targetfile(), table.join("-f", output, args:split(" ")))
			end)
		end
	end)
rule_end()
