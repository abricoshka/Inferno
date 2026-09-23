if has_config("tools") then
	target("inferno-fs-patcher")
		set_kind("binary")
		set_languages("c++17")
		add_files("main.cpp")
		add_cxxflags("-Wpedantic", "-Wconversion", "-Wsign-conversion", "-Wold-style-cast", "-Werror")
	target_end()
end
