target("system")
	if has_config("unimp") then
		add_files("unimp.c")
	end
	if has_config("empty_slot") then
		add_files("empty_slot.c")
	end
	if has_config("led") then
		add_files("led.c")
	end
	if has_config("aux") then
		add_files("auxbus.c")
	end
	if has_config("apple_soc") then
		add_files("aes.c", "a7iop/core.c", "a7iop/mailbox/core.c", "a7iop/mailbox/regs-v2.c", "a7iop/mailbox/regs-v4.c",
			"a7iop/regs-v2.c", "a7iop/regs-v4.c", "a7iop/rtkit.c", "smc.c", "roswell.c", "chestnut.c", "pmu-d2255.c",
			"aop.c", "baseband.c", "fan53740.c")
	end
	if has_config("apple_spmi_pmu") then
		add_files("spmi-pmu.c")
	end
	if has_config("apple_spmi_baseband") then
		add_files("spmi-baseband.c")
	end
target_end()
