if has_config("kvm") then
	target("qemu-" .. inferno_target)
		add_files("kvm-all.c", "kvm-accel-ops.c")
	target_end()
end
