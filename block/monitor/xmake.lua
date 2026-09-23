target("system")
	add_files("block-hmp-cmds.c")
target_end()

target("block")
	add_files("bitmap-qmp-cmds.c")
target_end()
