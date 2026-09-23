target("blockdev")
	add_files("export.c")
	if has_package("fuse") then
		add_files("fuse.c")
	end
target_end()
