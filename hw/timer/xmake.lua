target("system")
	if has_config("arm_timer") then
		add_files("arm_timer.c")
	end
target_end()
