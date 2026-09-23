target("system")
	add_files("core.c")
	if has_config("cpu_cluster") then
		add_files("cluster.c")
	end
target_end()
