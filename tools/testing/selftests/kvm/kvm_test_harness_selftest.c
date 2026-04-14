#include <stdio.h>

#include "kselftest_harness.h"
#include "test_util.h"

FIXTURE(my_fixture)
{

};

FIXTURE_SETUP(my_fixture)
{
	pr_info("setup\n");
}

FIXTURE_TEARDOWN(my_fixture)
{
	pr_info("teardown\n");
}

TEST_F(my_fixture, my_test_pass)
{
	TEST_ASSERT(true, "foobar");
}

TEST_F(my_fixture, my_test_assert)
{
	TEST_ASSERT(false, "foobar");
}

int main(int argc, char **argv)
{
	return test_harness_run(argc, argv);
}
