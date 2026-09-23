target("qmp")
	add_files("monitor.c", "qmp.c", "qmp-cmds-control.c")
target_end()

target("system")
	add_files("fds.c", "hmp-cmds.c", "hmp.c", "qemu-config-qmp.c", "qmp-cmds.c")
target_end()

target("qemu-" .. inferno_target)
	add_files("hmp-cmds-target.c", "hmp-target.c")
target_end()
