target("system")
	add_files("intc.c")
target_end()

if has_config("apple_soc") then
	target("qemu-" .. inferno_target)
		add_files("apple_aic.c")
	target_end()
end
