target("system")
	if has_config("usb") then
		add_files("bus.c", "combined-packet.c", "core.c", "desc.c", "libhw.c", "pcap.c")
	else
		add_files("bus-stub.c")
	end
	if has_config("apple_otg") then
		add_files("apple_otg.c")
	end
	if has_config("apple_typec") then
		add_files("apple_typec.c")
	end
	if has_config("usb_uplink") then
		add_files("dev-inferno-remote.c", "hcd-inferno.c", "usb-uplink.c", "hcd-virtualhere.c", "mdns.c")
	end
	if has_config("usb_ehci") then
		add_files("hcd-ehci.c")
	end
	if has_config("usb_ehci_sysbus") then
		add_files("hcd-ehci-sysbus.c")
	end
	if has_config("usb_xhci") then
		add_files("hcd-xhci.c")
	end
	if has_config("usb_xhci_sysbus") then
		add_files("hcd-xhci-sysbus.c")
	end
	if has_config("usb_dwc2") then
		add_files("hcd-dwc2.c")
	end
	if has_config("usb_dwc3") then
		add_files("hcd-dwc3.c")
	end
	if has_config("usb_hub") then
		add_files("dev-hub.c")
	end
	if has_config("usb") and has_package("usb_redir") then
		add_files("redirect.c", "quirks.c")
	end
	if has_config("usb") and has_package("libusb") then
		add_files("host-libusb.c")
	end
target_end()
