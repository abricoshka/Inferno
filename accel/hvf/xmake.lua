if has_config("hvf") then
	target("qemu-" .. inferno_target)
		add_files("hvf-all.c", "hvf-accel-ops.c")
	target_end()
end
