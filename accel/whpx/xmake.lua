if has_config("whpx") then
	target("qemu-" .. inferno_target)
		add_files("whpx-all.c", "whpx-accel-ops.c")
	target_end()
end
