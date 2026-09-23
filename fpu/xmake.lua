if has_config("tcg") then
	target("common")
		add_files("softfloat.c")
	target_end()
end
