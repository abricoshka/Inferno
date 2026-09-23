target("system")
	if has_config("apple_uart") then
		add_files("apple_uart.c")
	end
	if has_config("exynos4210_uart") then
		add_files("exynos4210_uart.c")
	end
target_end()
