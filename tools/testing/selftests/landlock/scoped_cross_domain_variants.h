/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Landlock self-tests - cross-domain scope variants
 *
 * Provides one FIXTURE_VARIANT and the four canonical combinations
 * (none->none, none->scoped, scoped->none, scoped->scoped). Every test that
 * checks interactions between two independently created domains
 * includes this header and iterates over the variants.
 *
 * Variant structure: which domain each side of the interaction lives in.
 *   resource_domain  - process that creates/owns the resource
 *   accessor_domain  - process that uses the resource
 *
 * Copyright © 2025 Abhinav Saxena <xandfury@gmail.com>
 *
 */

FIXTURE_VARIANT(cross_domain_scope)
{
	enum sandbox_type resource_domain;
	enum sandbox_type accessor_domain;
};

/* Four concrete combinations */
FIXTURE_VARIANT_ADD(cross_domain_scope, none_to_none) {
	.resource_domain = NO_SANDBOX,
	.accessor_domain = NO_SANDBOX,
};

FIXTURE_VARIANT_ADD(cross_domain_scope, none_to_scoped) {
	.resource_domain = NO_SANDBOX,
	.accessor_domain = SCOPE_SANDBOX,
};

FIXTURE_VARIANT_ADD(cross_domain_scope, scoped_to_none) {
	.resource_domain = SCOPE_SANDBOX,
	.accessor_domain = NO_SANDBOX,
};

FIXTURE_VARIANT_ADD(cross_domain_scope, scoped_to_scoped) {
	.resource_domain = SCOPE_SANDBOX,
	.accessor_domain = SCOPE_SANDBOX,
};

/*
 * Mapping reminder:
 *   SIGNAL               resource = receiver    accessor = sender
 *   ABSTRACT UNIX        resource = server      accessor = client
 *   future scopes        resource = creator     accessor = user
 *
 * Only the accessor domain is enforced; tests therefore expect:
 *   accessor NO_SANDBOX      -> ALLOW operation
 *   accessor SCOPE_SANDBOX   -> DENY if resource is outside its domain
 */
