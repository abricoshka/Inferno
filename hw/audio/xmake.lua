target("system")
	if has_config("apple_soc") then
		add_files("cs35l27.c", "cs42l77.c", "aop-audio.c", "mca.c")
	end
target_end()
