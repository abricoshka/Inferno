import("core.base.option")
import("core.project.config")
import("core.project.depend")
import("lib.detect.find_tool")
import("utils.progress")

function gendir()
	return path.absolute(path.join(config.builddir(), ".gens", "inferno"), os.projectdir())
end

function srcfile(...)
	return path.join(os.projectdir(), ...)
end

function python()
	local tool = find_tool("python3") or find_tool("python")
	assert(tool, "python3 not found")
	return tool.program
end

function write_if_changed(file, content)
	if not os.isfile(file) or io.readfile(file) ~= content then
		os.mkdir(path.directory(file))
		io.writefile(file, content)
	end
end

function generate(target, name, opt, fn)
	local missing = false
	for _, output in ipairs(opt.outputs) do
		if not os.isfile(output) then
			missing = true
			break
		end
	end
	depend.on_changed(function ()
		local label = opt.label or path.filename(opt.outputs[1])
		if opt.progress then
			progress.show(opt.progress, "${color.build.object}generating.%s %s", name, label)
		else
			cprint("${color.build.object}generating.%s %s", name, label)
		end
		for _, output in ipairs(opt.outputs) do
			os.mkdir(path.directory(output))
		end
		fn()
	end, {
		dependfile = target:dependfile(path.join("inferno", name, opt.label or path.filename(opt.outputs[1]))),
		files = opt.inputs,
		values = opt.values,
		changed = missing or target:is_rebuilt(),
	})
end

function capture(program, argv, output)
	os.mkdir(path.directory(output))
	os.execv(program, argv, {stdout = output})
end

function jobs()
	return tonumber(option.get("jobs")) or os.default_njob()
end

function entitle(file)
	local argv = {srcfile("scripts", "entitlement.sh"), file, srcfile("pc-bios", "qemu.rsrc")}
	if config.get("hvf") then
		table.insert(argv, srcfile("accel", "hvf", "entitlements.plist"))
	end
	os.vrunv("sh", argv)
end
