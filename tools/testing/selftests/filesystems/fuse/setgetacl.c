// SPDX-License-Identifier: GPL-2.0

/* Simple ACL set/get wrapper */

#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/acl.h>

int main(int argc, char *argv[])
{
	acl_t acl;
	acl_type_t type;
	char *buf;

	if ((argc != 3) && (argc != 4)) {
		fprintf(stderr, "Usage: %s <acl type> [<value>] <file>\n",
			argv[0]);
		fprintf(stderr, "where <acl type> is ACCESS or DEFAULT\n");
		return 1;
	}

	if (!strcmp(argv[1], "ACCESS"))
		type = ACL_TYPE_ACCESS;
	else if (!strcmp(argv[1], "DEFAULT"))
		type = ACL_TYPE_DEFAULT;
	else {
		fprintf(stderr, "Invalid ACL type\n");
		return 1;
	}

	if (argc == 3) {
		/* Get ACL */
		acl = acl_get_file(argv[2], type);
		if (acl == NULL) {
			perror("acl_get_file");
			return 1;
		}
		buf = acl_to_text(acl, NULL);
		if (buf == NULL)
			perror("acl_from_text");
		else {
			fprintf(stdout, "%s\n", buf);
			acl_free(buf);
		}
	} else {
		/* Set ACL */
		acl = acl_from_text(argv[2]);
		if (acl == NULL) {
			perror("acl_from_text");
			return 1;
		}
		if (acl_set_file(argv[3], type, acl) != 0)
			perror("acl_set_file");
	}

	acl_free(acl);

	return 0;
}
