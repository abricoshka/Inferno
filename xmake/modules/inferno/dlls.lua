import("lib.detect.find_tool")

local function objdump_program(target)
	local cc = target:tool("cc")
	local name = path.basename(cc)
	local prefix = name:match("^(.*)gcc$") or name:match("^(.*)cc$")
	local candidates = prefix and {prefix .. "objdump", "objdump"} or {"llvm-objdump", "objdump"}
	local dir = path.directory(cc)
	local paths = dir ~= "." and {dir} or nil
	for _, candidate in ipairs(candidates) do
		local tool = find_tool(candidate, {program = candidate, paths = paths})
		if tool then
			return tool.program
		end
	end
	raise("objdump not found, cannot collect runtime DLLs")
end

local function imports(objdump, file)
	local names = {}
	for line in os.iorunv(objdump, {"-p", file}):gmatch("[^\r\n]+") do
		local name = line:match("DLL Name:%s*(%S+)")
		if name then
			table.insert(names, name)
		end
	end
	return names
end

local function packages(target)
	local pkgs = {}
	for _, pkg in ipairs(target:orderpkgs()) do
		table.insert(pkgs, pkg)
	end
	for _, dep in ipairs(target:orderdeps()) do
		for _, pkg in ipairs(dep:orderpkgs({interface = true})) do
			table.insert(pkgs, pkg)
		end
	end
	return pkgs
end

local function collect(target)
	local dlls = {}
	local dirs = {}
	local function add(dir)
		if dir and os.isdir(dir) and not table.contains(dirs, dir) then
			table.insert(dirs, dir)
		end
	end
	for _, pkg in ipairs(packages(target)) do
		if pkg:enabled() then
			for _, libfile in ipairs(table.wrap(pkg:get("libfiles"))) do
				if path.filename(libfile):lower():endswith(".dll") then
					table.insert(dlls, libfile)
					add(path.directory(libfile))
				end
			end
			for _, dir in ipairs(table.wrap(pkg:get("linkdirs"))) do
				add(path.join(path.directory(dir), "bin"))
				add(dir)
			end
		end
	end
	local toolchain = target:toolchain("mingw")
	if toolchain then
		add(toolchain:bindir())
	end
	return dlls, dirs
end

function bundle(target, destdir)
	local objdump = objdump_program(target)
	local seeds, dirs = collect(target)
	local queue = {target:targetfile()}
	local seen = {}
	os.mkdir(destdir)
	local function place(dll)
		local dest = path.join(destdir, path.filename(dll))
		if not os.isfile(dest) or os.filesize(dest) ~= os.filesize(dll) then
			os.vcp(dll, dest)
		end
		table.insert(queue, dll)
	end
	for _, dll in ipairs(seeds) do
		seen[path.filename(dll):lower()] = true
		place(dll)
	end
	while #queue > 0 do
		local file = table.remove(queue)
		for _, name in ipairs(imports(objdump, file)) do
			local key = name:lower()
			if not seen[key] then
				seen[key] = true
				for _, dir in ipairs(dirs) do
					local dll = path.join(dir, name)
					if os.isfile(dll) then
						place(dll)
						break
					end
				end
			end
		end
	end
end
