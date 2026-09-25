if has_config("tools") then
	target("inferno-fs-patcher")
		set_kind("binary")
		set_languages("gnu11", "c++17")
		add_deps("genh", "authz", "block", "crypto", "io", "qom", "qemuutil")
		add_files("main.cpp", "disk.c", "../hw/arm/apfs.c", "../hw/arm/libzbitmap.c")
		add_cxxflags("-Wpedantic", "-Wconversion", "-Wsign-conversion", "-Wold-style-cast", "-Werror")
		if is_plat("macosx") then
			add_frameworks("CoreFoundation", "IOKit")
		end
	target_end()
end
