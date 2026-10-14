target("system")
	if has_config("apple_soc") then
		add_files("apple_apt_m3.c", "apple_displaypipe_v4.c", "apple_displaypipe_v2.c", "synopsys_mipi_dsim.c", "apple_scaler.c")
	end
	if has_config("ddc") then
		add_files("i2c-ddc.c")
	end
	if has_config("edid") then
		add_files("edid-generate.c", "edid-region.c")
	end
	if has_config("framebuffer") then
		add_files("framebuffer.c")
	end
	if has_config("dpcd") then
		add_files("dpcd.c")
	end
target_end()
