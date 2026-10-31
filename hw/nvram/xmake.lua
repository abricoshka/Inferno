target("system")
	if has_config("chrp_nvram") then
		add_files("chrp_nvram.c")
	end
	if has_config("apple_nvram") then
		add_files("apple_nvram.c")
	end
target_end()
