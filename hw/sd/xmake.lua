target("system")
	if has_config("sd") then
		add_files("sd.c", "core.c")
	end
	if has_config("sdhci") then
		add_files("sdhci.c")
	end
	if has_config("sdhci_pci") then
		add_files("sdhci-pci.c")
	end
	if has_config("ssi_sd") then
		add_files("ssi-sd.c")
	end
target_end()
