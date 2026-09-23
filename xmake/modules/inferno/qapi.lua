import("inferno.codegen")

function outdir()
	return path.join(codegen.gendir(), "qapi")
end

function modules(genh)
	return table.wrap(genh:values("inferno.qapi_modules"))
end

function outputs(genh)
	local names = {
		"qapi-introspect", "qapi-types", "qapi-visit", "qapi-commands", "qapi-init-commands", "qapi-events",
		"qapi-emit-events", "qapi-builtin-types", "qapi-builtin-visit",
	}
	for _, module in ipairs(modules(genh)) do
		for _, kind in ipairs({"types", "visit", "events", "commands"}) do
			table.insert(names, "qapi-" .. kind .. "-" .. module)
		end
	end
	local files = {}
	for _, name in ipairs(names) do
		table.insert(files, path.join(outdir(), name .. ".c"))
		table.insert(files, path.join(outdir(), name .. ".h"))
	end
	return files
end

function sources(genh)
	local files = {}
	for _, file in ipairs(outputs(genh)) do
		if file:endswith(".c") then
			table.insert(files, file)
		end
	end
	return files
end

function trace_events(genh)
	local files = {}
	for _, module in ipairs(modules(genh)) do
		table.insert(files, path.join(outdir(), "qapi-commands-" .. module .. ".trace-events"))
	end
	return files
end

function generate(genh, opt)
	local schema = codegen.srcfile("qapi", "qapi-schema.json")
	local inputs = table.join(os.files(codegen.srcfile("qapi", "*.json")), os.files(codegen.srcfile("scripts", "qapi", "*.py")),
		codegen.srcfile("scripts", "qapi-gen.py"))
	codegen.generate(genh, "qapi", {
		progress = opt.progress,
		label = "qapi-schema.json",
		inputs = inputs,
		values = modules(genh),
		outputs = table.join(outputs(genh), trace_events(genh)),
	}, function ()
		os.vrunv(codegen.python(), {codegen.srcfile("scripts", "qapi-gen.py"), "-o", outdir(), "-b", schema})
	end)
end
