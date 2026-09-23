import("core.project.config")
import("core.project.project")
import("async.runjobs")
import("inferno.codegen")
import("inferno.qapi")

function outdir()
	return path.join(codegen.gendir(), "trace")
end

function backends()
	return (config.get("trace_backends") or "nop"):split(",")
end

function has_backend(name)
	return table.contains(backends(), name)
end

function events(genh)
	local list = {{group = "root", file = codegen.srcfile("trace-events")}}
	for _, dir in ipairs(table.wrap(genh:values("inferno.trace_dirs"))) do
		for _, file in ipairs(os.files(codegen.srcfile(dir, "**trace-events"))) do
			local subdir = path.relative(path.directory(file), os.projectdir())
			table.insert(list, {group = subdir:gsub("[^%w]", "_"), file = file})
		end
	end
	for _, file in ipairs(qapi.trace_events(genh)) do
		table.insert(list, {group = path.filename(file):gsub("[^%w]", "_"), file = file})
	end
	return list
end

function sources(genh)
	local files = {}
	for _, event in ipairs(events(genh)) do
		table.insert(files, path.join(outdir(), "trace-" .. event.group .. ".c"))
		if has_backend("dtrace") and not is_plat("macosx") then
			table.insert(files, path.join(outdir(), "trace-dtrace-" .. event.group .. ".o"))
		end
	end
	if has_backend("ust") then
		table.insert(files, path.join(outdir(), "trace-ust-all.c"))
	end
	return files
end

function _tracetool(group, format, input, output)
	os.vrunv(codegen.python(), {codegen.srcfile("scripts", "tracetool.py"), "--backend=" .. table.concat(backends(), ","),
		"--group=" .. group, "--format=" .. format, input, output})
end

function generate(genh, opt)
	local tools = os.files(codegen.srcfile("scripts", "tracetool", "**.py"))
	local list = events(genh)
	runjobs("trace", function (index)
		local event = list[index]
		local prefix = path.join(outdir(), "trace-")
		local outputs = {prefix .. event.group .. ".h", prefix .. event.group .. ".c"}
		if has_backend("dtrace") then
			table.insert(outputs, prefix .. "dtrace-" .. event.group .. ".h")
			if not is_plat("macosx") then
				table.insert(outputs, prefix .. "dtrace-" .. event.group .. ".o")
			end
		end
		if has_backend("ust") then
			table.insert(outputs, prefix .. "ust-" .. event.group .. ".h")
		end
		codegen.generate(genh, "trace", {
			progress = opt.progress,
			inputs = table.join(event.file, tools),
			values = backends(),
			outputs = outputs,
		}, function ()
			_tracetool(event.group, "h", event.file, prefix .. event.group .. ".h")
			_tracetool(event.group, "c", event.file, prefix .. event.group .. ".c")
			if has_backend("dtrace") then
				local script = prefix .. "dtrace-" .. event.group .. ".dtrace"
				_tracetool(event.group, "d", event.file, script)
				os.vrunv("dtrace", {"-DSTAP_SDT_V2", "-o", prefix .. "dtrace-" .. event.group .. ".h", "-h", "-s", script})
				if not is_plat("macosx") then
					os.vrunv("dtrace", {"-DSTAP_SDT_V2", "-o", prefix .. "dtrace-" .. event.group .. ".o", "-G", "-s", script})
				end
			end
			if has_backend("ust") then
				_tracetool(event.group, "ust-events-h", event.file, prefix .. "ust-" .. event.group .. ".h")
			end
		end)
	end, {total = #list, comax = codegen.jobs()})

	local files = {}
	for _, event in ipairs(list) do
		table.insert(files, event.file)
	end
	if has_backend("ust") then
		local prefix = path.join(outdir(), "trace-ust-all.")
		codegen.generate(genh, "trace", {
			progress = opt.progress,
			label = "trace-ust-all",
			inputs = table.join(files, tools),
			values = backends(),
			outputs = {prefix .. "h", prefix .. "c"},
		}, function ()
			for _, ext in ipairs({"h", "c"}) do
				os.vrunv(codegen.python(), table.join({codegen.srcfile("scripts", "tracetool.py"),
					"--backend=" .. table.concat(backends(), ","), "--group=all", "--format=ust-events-" .. ext},
					files, prefix .. ext))
			end
		end)
	end
	if not (#backends() == 1 and has_backend("nop")) then
		local content = {}
		for _, file in ipairs(files) do
			table.insert(content, io.readfile(file))
		end
		codegen.write_if_changed(path.join(codegen.gendir(), "trace-events-all"), table.concat(content))
	end
	_tapsets(genh, opt, tools)
end

function tapsets(name)
	local prefix = path.join(codegen.gendir(), "systemtap", name)
	return {
		{output = prefix .. ".stp-build", format = "stap", installed = false},
		{output = prefix .. ".stp", format = "stap", installed = true},
		{output = prefix .. "-simpletrace.stp", format = "simpletrace-stap", installed = true},
		{output = prefix .. "-log.stp", format = "log-stap", installed = true},
	}
end

function _tapsets(genh, opt, tools)
	local events_all = path.join(codegen.gendir(), "trace-events-all")
	for _, name in ipairs(table.wrap(genh:values("inferno.traceable"))) do
		local target = project.target(name)
		local probe_prefix = name:gsub("-", ".")
		for _, tapset in ipairs(tapsets(name)) do
			local binary = ""
			if tapset.format == "stap" then
				binary = tapset.installed and path.join(config.get("prefix"), "bin", target:filename()) or
					path.absolute(target:targetfile())
			end
			codegen.generate(genh, "tapset", {
				progress = opt.progress,
				inputs = table.join(events_all, tools),
				values = {binary, probe_prefix, backends()},
				outputs = {tapset.output},
			}, function ()
				os.vrunv(codegen.python(), {codegen.srcfile("scripts", "tracetool.py"),
					"--backend=" .. table.concat(backends(), ","), "--group=all", "--format=" .. tapset.format,
					"--binary=" .. binary, "--probe-prefix=" .. probe_prefix, events_all, tapset.output})
			end)
		end
	end
end
