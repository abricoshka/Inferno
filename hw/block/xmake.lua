target("system")
	add_files("block.c")
	if has_config("apple_ans") then
		add_files("ans.c")
	end
	if has_config("apple_nvme_mmu") then
		add_files("nvme_mmu.c")
	end
target_end()
