if has_config("pci") and has_config("apple_soc") then
	target("system")
		add_files("apcie.c")
	target_end()
end
