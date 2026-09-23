target("system")
	add_files("hostmem-ram.c", "hostmem.c", "rng-builtin.c", "rng-egd.c", "rng.c")
	if not is_plat("mingw") then
		add_files("rng-random.c")
		if not is_plat("wasm") then
			add_files("hostmem-file.c", "hostmem-shm.c")
		end
	end
	if is_plat("linux") then
		add_files("hostmem-memfd.c", "host_iommu_device.c")
	end
target_end()
