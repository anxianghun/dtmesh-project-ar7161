/*
 * hostapd - Plaintext password to NtPasswordHash
 * Copyright (c) 2005, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "includes.h"

#include "common.h"
#include "ms_funcs.h"


int main(int argc, char *argv[])
{
	unsigned char password_hash[16];
	size_t i;
	char *password, buf[64], *pos;

	if (argc > 1)
		password = argv[1];
	else {
		if (fgets(buf, sizeof(buf), stdin) == NULL) {
			printf("Failed to read password\n");
			return 1;
		}
                pos = buf+strlen(buf);
                while (pos > buf && !isgraph(pos[-1])) *--pos = '\0';
		/* if (pos == buf || buf[0] == '#') continue; */
		password = buf;
	}

	nt_password_hash((u8 *) password, strlen(password), password_hash);
	for (i = 0; i < sizeof(password_hash); i++)
		printf("%02x", password_hash[i]);
	printf("\n");

	return 0;
}