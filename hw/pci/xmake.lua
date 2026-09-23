target("system")
	if has_config("pci") then
		add_files("msi.c", "msix.c", "pci.c", "pci_bridge.c", "pci_host.c", "pci-hmp-cmds.c", "pci-qmp-cmds.c", "shpc.c",
			"slotid_cap.c", "pcie.c", "pcie_aer.c")
	else
		add_files("pci-stub.c")
	end
	if has_config("pci_express") then
		add_files("pcie_port.c", "pcie_host.c")
	end
target_end()
