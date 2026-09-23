if has_config("apple_soc") then
	target("system_arm")
		add_files("aes.c", "boot-monitor.c", "debug-trace.c", "eisp.c", "key.c", "misc.c", "monitor.c", "pka.c",
			"pmgr.c", "progress.c", "ssc.c", "trng.c", "sim.c", "emu.c")
	target_end()
end
