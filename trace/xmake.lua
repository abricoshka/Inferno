target("system")
	add_files("control-target.c", "trace-hmp-cmds.c")
target_end()

target("qemuutil")
	add_files("control.c", "qmp.c")
	if table.contains(inferno_trace_backends, "simple") then
		add_files("simple.c")
	end
	if table.contains(inferno_trace_backends, "ftrace") then
		add_files("ftrace.c")
	end
target_end()
