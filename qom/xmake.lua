target("qom")
	add_files("container.c", "object.c", "object_interfaces.c", "qom-qobject.c")
target_end()

target("qmp")
	add_files("qom-qmp-cmds.c")
target_end()

target("system")
	add_files("qom-hmp-cmds.c")
target_end()
