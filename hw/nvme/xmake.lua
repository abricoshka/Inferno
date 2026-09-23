target("system")
	if has_config("nvme_pci") then
		add_files("ctrl.c", "dif.c", "ns.c", "subsys.c", "nguid.c")
	end
target_end()
