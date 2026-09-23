import("core.project.config")
import("inferno.codegen")

local hx_headers = {
	{"qemu-options.hx", "qemu-options.def"},
	{"qemu-img-cmds.hx", "qemu-img-cmds.h"},
	{"hmp-commands.hx", "hmp-commands.h"},
	{"hmp-commands-info.hx", "hmp-commands-info.h"},
}

local keymaps = {
	{"atset1", "qcode"},
	{"linux", "qcode"},
	{"qcode", "qnum"},
	{"qnum", "qcode"},
	{"usb", "qcode"},
	{"x11", "qcode"},
	{"xorgevdev", "qcode"},
	{"xorgkbd", "qcode"},
	{"xorgxquartz", "qcode"},
	{"xorgxwin", "qcode"},
	{"osx", "qcode"},
}

function version()
	return io.readfile(codegen.srcfile("VERSION")):trim()
end

function _hx(genh, opt)
	local hxtool = codegen.srcfile("scripts", "hxtool")
	for _, hx in ipairs(hx_headers) do
		local input = codegen.srcfile(hx[1])
		local output = path.join(codegen.gendir(), hx[2])
		codegen.generate(genh, "hx", {progress = opt.progress, inputs = {input, hxtool}, outputs = {output}}, function ()
			codegen.capture("sh", {hxtool, "-h", input}, output)
		end)
	end
end

function _keymaps(genh, opt)
	local keycodemapdb = genh:pkg("keycodemapdb"):installdir()
	local keymap_gen = path.join(keycodemapdb, "tools", "keymap-gen")
	local keymaps_csv = path.join(keycodemapdb, "data", "keymaps.csv")
	for _, keymap in ipairs(keymaps) do
		local name = string.format("input-keymap-%s-to-%s", keymap[1], keymap[2])
		local output = path.join(codegen.gendir(), "ui", name .. ".c.inc")
		codegen.generate(genh, "keymap", {progress = opt.progress, inputs = {keymap_gen, keymaps_csv}, outputs = {output}}, function ()
			codegen.capture(codegen.python(), {keymap_gen, "code-map", "--lang", "glib2", "--varname",
				string.format("qemu_input_map_%s_to_%s", keymap[1], keymap[2]), keymaps_csv, keymap[1], keymap[2]}, output)
		end)
	end
end

function _version()
	local output = os.iorunv("sh", {codegen.srcfile("scripts", "qemu-version.sh"), os.projectdir(),
		config.get("pkgversion") or "", version()})
	codegen.write_if_changed(path.join(codegen.gendir(), "qemu-version.h"), output)
end

function _poison(genh)
	local headers = {}
	for _, name in ipairs({"config-target.h", "config-devices.h"}) do
		table.insert(headers, path.join(codegen.gendir(), genh:values("inferno.target") .. "-" .. name))
	end
	local output = os.iorunv("sh", table.join(codegen.srcfile("scripts", "make-config-poison.sh"), headers))
	codegen.write_if_changed(path.join(codegen.gendir(), "config-poison.h"), output)
end

function generate(genh, opt)
	_hx(genh, opt)
	_keymaps(genh, opt)
	_version()
	_poison(genh)
end
