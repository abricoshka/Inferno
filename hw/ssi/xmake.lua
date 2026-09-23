target("system")
	if has_config("ssi") then
		add_files("ssi.c")
	end
	if has_config("apple_spi") then
		add_files("apple_spi.c")
	end
target_end()
