target("system")
	add_files("watchdog.c")
	if has_config("apple_soc") then
		add_files("apple_wdt.c")
	end
target_end()
