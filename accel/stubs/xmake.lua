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
	add_files("whpx-stub.c")
target_end()
