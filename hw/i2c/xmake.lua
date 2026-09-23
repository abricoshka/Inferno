if has_config("i2c") then
	target("system")
		add_files("core.c")
		if has_config("exynos4210_uart") then
			add_files("exynos4210_i2c.c")
		end
		if has_config("apple_soc") and has_config("apple_i2c") then
			add_files("apple_i2c.c")
		end
	target_end()
end
