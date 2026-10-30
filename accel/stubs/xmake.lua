target("qemu-" .. inferno_target)
	if not has_config("kvm") then
		add_files("kvm-stub.c")
	end
	if not has_config("tcg") then
		add_files("tcg-stub.c")
	end
	if not has_config("hvf") then
		add_files("hvf-stub.c")
	end
	if not has_config("whpx") then
		add_files("whpx-stub.c")
	end
target_end()
