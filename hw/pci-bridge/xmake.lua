if has_config("pci") and has_config("pcie_port") then
	target("system")
		add_files("pcie_root_port.c")
	target_end()
end
