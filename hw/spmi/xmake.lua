if has_config("spmi") then
	target("system")
		add_files("core.c")
		if has_config("apple_spmi") then
			add_files("apple_spmi.c")
		end
	target_end()
end
