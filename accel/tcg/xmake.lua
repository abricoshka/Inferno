if has_config("tcg") then
	target("system")
		add_files("cpu-exec.c", "cpu-exec-common.c", "tcg-runtime.c", "tcg-runtime-gvec.c", "tb-maint.c", "tcg-all.c",
			"tcg-stats.c", "translate-all.c", "translator.c")
		add_files("cputlb.c", "monitor.c", "tcg-accel-ops.c", "tcg-accel-ops-mttcg.c", "tcg-accel-ops-rr.c", "watchpoint.c")
	target_end()
end
