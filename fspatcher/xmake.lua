if has_config("tools") then
	add_requires("cli11", {external = true})
	target("inferno-fs-patcher")
		set_kind("binary")
		set_languages("gnu11", "c++17")
		add_deps("genh", "authz", "block", "crypto", "fs", "io", "qom", "qemuutil")
		add_packages("cli11")
		add_files("main.cpp", "disk.c")
		add_cxxflags("-Wpedantic", "-Wconversion", "-Wsign-conversion", "-Wold-style-cast", "-Werror")
		if is_plat("macosx") then
			add_frameworks("CoreFoundation", "IOKit")
		end
	target_end()
end
