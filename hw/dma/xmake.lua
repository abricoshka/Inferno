target("system")
	if has_config("apple_sio") then
		add_files("apple_sio.c")
	end
target_end()
