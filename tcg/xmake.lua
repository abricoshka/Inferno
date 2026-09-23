if has_config("tcg") then
	target("system")
		add_files("optimize.c", "region.c", "tcg.c", "tcg-common.c", "tcg-op.c", "tcg-op-ldst.c", "tcg-op-gvec.c",
			"tcg-op-vec.c")
		if has_config("tcg_interpreter") then
			add_files("tci.c")
		end
	target_end()
end
