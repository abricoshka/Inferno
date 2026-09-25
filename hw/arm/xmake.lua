target("system_arm")
	if has_config("apple_dart") then
		add_files("dart.c")
	else
		add_files("dart-stub.c")
	end
	if has_config("apple_sart") then
		add_files("sart.c")
	end
	if has_config("apple_soc") then
		add_files("a13.c", "a13_gxf.c", "a9.c", "apfs.c", "boot.c", "dt.c", "hfs.c", "libzbitmap.c", "mem.c", "patcher.c", "kernel_patches.c",
			"lm-backlight.c", "s8000.c", "t8030.c")
	end
target_end()

includes("sep")
