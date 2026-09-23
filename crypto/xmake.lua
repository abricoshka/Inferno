target("crypto")
	add_files("afsplit.c", "akcipher.c", "block-luks.c", "block-qcow.c", "block.c", "cipher.c", "der.c", "hash.c",
		"hmac.c", "ivgen-essiv.c", "ivgen-plain.c", "ivgen-plain64.c", "ivgen.c", "pbkdf.c", "secret_common.c",
		"secret.c", "tlscreds.c", "tlscredsanon.c", "tlscredspsk.c", "tlscredsx509.c", "tlssession.c", "rsakey.c",
		"hash-nettle.c", "hmac-nettle.c", "pbkdf-nettle.c")
	if has_package("gnutls") then
		add_files("x509-utils.c")
	end
	if not has_config("__nettle_xts") then
		add_files("xts.c")
	end
	if has_config("__keyring") then
		add_files("secret_keyring.c")
	end
target_end()

target("system")
	if has_package("gnutls") then
		add_files("tls-cipher-suites.c")
	end
target_end()

target("qemuutil")
	add_files("aes.c", "clmul.c", "init.c", "sm4.c")
	if has_package("gnutls") then
		add_files("random-gnutls.c")
	else
		add_files("random-platform.c")
	end
target_end()
