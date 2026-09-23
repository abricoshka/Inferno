target("qemu-" .. inferno_target)
	add_files("arch_init.c", "globals-target.c")
target_end()

target("system")
	add_files("vl.c", "cpus.c", "cpu-timers.c", "datadir.c", "dma-helpers.c", "globals.c", "ioport.c",
		"ram-block-attributes.c", "memory_mapping.c", "memory.c", "physmem.c", "qdev-monitor.c", "rtc.c",
		"runstate-action.c", "runstate-hmp-cmds.c", "runstate.c", "watchpoint.c")
	if has_package("seccomp") then
		add_files("qemu-seccomp.c")
	end
	if is_plat("linux") then
		add_files("async-teardown.c")
	end
target_end()
