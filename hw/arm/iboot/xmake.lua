if has_config("apple_soc") then
	target("system_arm")
		add_files("boot.c", "dfu.c", "iboot.c", "kernel_patches.c", "patcher.c")
	target_end()
end
