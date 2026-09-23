target("system")
	if has_config("gpio_key") then
		add_files("gpio_key.c")
	end
	if has_config("gpio_pwr") then
		add_files("gpio_pwr.c")
	end
	if has_config("apple_soc") then
		add_files("apple_gpio.c")
	end
target_end()
