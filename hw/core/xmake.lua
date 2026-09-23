target("hwcore")
	add_files("bus.c", "qdev-properties.c", "qdev.c", "resetcontainer.c", "resettable.c", "irq.c", "clock.c",
		"qdev-clock.c")
target_end()

target("common")
	add_files("cpu-common.c", "machine-smp.c")
target_end()

target("system")
	if has_config("or_irq") then
		add_files("or-irq.c")
	end
	if has_config("ptimer") then
		add_files("ptimer.c")
	end
	if has_config("register") then
		add_files("register.c")
	end
	if has_config("split_irq") then
		add_files("split-irq.c")
	end
	add_files("cpu-system.c", "fw-path-provider.c", "gpio.c", "hotplug.c", "machine-hmp-cmds.c", "machine-qmp-cmds.c",
		"machine.c", "nmi.c", "qdev-fw.c", "qdev-hotplug.c", "qdev-properties-system.c", "reset.c", "sysbus.c",
		"vm-change-state-handler.c")
target_end()
