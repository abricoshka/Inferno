target("system")
	if has_config("apple_soc") then
		add_files("buttons.c", "mt-spi.c")
	end
target_end()
