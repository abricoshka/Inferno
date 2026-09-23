local devices = {
	{"apple_soc", "Apple SoC machines (T8030, S8000)", default = true, depends = {"tcg"}, selects = {
		"at24c", "i2c", "pci", "pci_devices", "pci_express", "pcie_port", "exynos4210_uart", "or_irq",
		"unimp", "apple_ans", "apple_nvme_mmu", "apple_i2c", "apple_otg", "apple_typec", "apple_nvram",
		"apple_sio", "apple_spmi", "apple_spmi_pmu", "apple_spmi_baseband", "apple_dart", "apple_sart",
		"apple_spi", "apple_uart", "framebuffer", "cpu_cluster"}},
	{"apple_ans", "Apple ANS NVMe storage controller", depends = {"apple_soc"}, selects = {"nvme_pci"}},
	{"apple_dart", "Apple DART IOMMU", depends = {"apple_soc"}},
	{"apple_i2c", "Apple I2C controller", depends = {"apple_soc"}},
	{"apple_nvme_mmu", "Apple NVMe MMU", depends = {"apple_soc"}, selects = {"nvme_pci"}},
	{"apple_nvram", "Apple NVRAM", depends = {"apple_soc"}, selects = {"chrp_nvram"}},
	{"apple_otg", "Apple USB OTG", depends = {"apple_soc"}, selects = {"usb_uplink", "usb_dwc2"}},
	{"apple_sart", "Apple SART address filter", depends = {"apple_soc"}},
	{"apple_sio", "Apple SIO DMA controller"},
	{"apple_spi", "Apple SPI controller", depends = {"apple_sio"}, selects = {"ssi"}},
	{"apple_spmi", "Apple SPMI controller", selects = {"spmi"}},
	{"apple_spmi_baseband", "Apple SPMI baseband", depends = {"apple_soc", "spmi"}},
	{"apple_spmi_pmu", "Apple SPMI PMU", depends = {"apple_soc", "spmi"}},
	{"apple_typec", "Apple USB Type-C", depends = {"apple_soc"}, selects = {"usb_uplink", "usb_dwc3"}},
	{"apple_uart", "Apple UART"},
	{"arm_timer", "ARM dual timer", selects = {"ptimer"}},
	{"at24c", "AT24C EEPROM", depends = {"i2c"}},
	{"aux", "DisplayPort AUX bus", selects = {"i2c"}},
	{"chrp_nvram", "CHRP NVRAM partitions"},
	{"cpu_cluster", "CPU cluster container"},
	{"ddc", "DDC display channel", depends = {"i2c"}, selects = {"edid"}},
	{"dpcd", "DisplayPort configuration data", selects = {"aux"}},
	{"edid", "EDID generator"},
	{"empty_slot", "Empty bus slot"},
	{"exynos4210_uart", "Exynos4210 UART"},
	{"framebuffer", "Framebuffer helpers"},
	{"gpio_key", "GPIO key"},
	{"gpio_pwr", "GPIO power controller"},
	{"i2c", "I2C bus"},
	{"led", "LED"},
	{"nvme_pci", "NVMe PCI controller", default_if = {"pci_devices"}, depends = {"pci"}},
	{"or_irq", "OR IRQ gate"},
	{"pci", "PCI bus", selected_if = {"kvm"}},
	{"pci_devices", "Pluggable PCI devices"},
	{"pci_express", "PCI Express", selects = {"pci"}},
	{"pcie_port", "PCI Express root port", default_if = {"pci_devices"}, depends = {"pci_express"}},
	{"ptimer", "Periodic timer helper"},
	{"register", "Register API"},
	{"sd", "SD card"},
	{"sdhci", "SD host controller", selects = {"sd"}},
	{"sdhci_pci", "SD host controller on PCI", default_if = {"pci_devices"}, depends = {"pci"}, selects = {"sdhci"}},
	{"split_irq", "IRQ splitter"},
	{"spmi", "SPMI bus"},
	{"ssi", "SSI bus"},
	{"ssi_sd", "SD card on SSI", depends = {"ssi"}, selects = {"sd"}},
	{"unimp", "Unimplemented device placeholder"},
	{"usb", "USB bus"},
	{"usb_dwc2", "Synopsys DWC2 USB controller", selects = {"usb"}},
	{"usb_dwc3", "Synopsys DWC3 USB controller", selects = {"usb_xhci_sysbus", "register"}},
	{"usb_ehci", "EHCI USB controller", selects = {"usb"}},
	{"usb_ehci_sysbus", "EHCI USB controller on sysbus", selects = {"usb_ehci"}},
	{"usb_hub", "USB hub", default = true, depends = {"usb"}},
	{"usb_uplink", "USB uplink to remote hosts", default = true, depends = {"usb"}},
	{"usb_xhci", "xHCI USB controller", selects = {"usb"}},
	{"usb_xhci_sysbus", "xHCI USB controller on sysbus", selects = {"usb_xhci"}},
}

local selected_by = {}
for _, device in ipairs(devices) do
	for _, selected in ipairs(device.selects or {}) do
		selected_by[selected] = selected_by[selected] or {}
		table.insert(selected_by[selected], device[1])
	end
end

inferno_devices = {}
for _, device in ipairs(devices) do
	local name = device[1]
	local default = device.default == true
	local depends = device.depends or {}
	local default_if = device.default_if or {}
	local selectors = table.join(selected_by[name] or {}, device.selected_if or {})
	table.insert(inferno_devices, name)
	option(name)
		set_default(default)
		set_showmenu(true)
		set_category("Devices")
		set_description(device[2])
		set_configvar("CONFIG_" .. name:upper(), 1)
		local deps = table.join(depends, default_if, selectors)
		if #deps > 0 then
			add_deps(table.unpack(deps))
		end
		after_check(function (option)
			import("core.project.config")
			local enabled = default
			if config.readonly(name) then
				enabled = option:enabled()
			else
				for _, dep in ipairs(default_if) do
					enabled = enabled or option:dep(dep):enabled()
				end
			end
			for _, dep in ipairs(depends) do
				enabled = enabled and option:dep(dep):enabled()
			end
			for _, dep in ipairs(selectors) do
				enabled = enabled or option:dep(dep):enabled()
			end
			option:enable(enabled == true, {force = true})
		end)
	option_end()
end
