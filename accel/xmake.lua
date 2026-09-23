target("common")
	add_files("accel-common.c")
target_end()

target("system")
	add_files("accel-system.c", "accel-blocker.c", "accel-qmp.c")
target_end()

target("qemu-" .. inferno_target)
	add_files("accel-target.c")
target_end()

includes("tcg", "hvf", "kvm", "stubs")
