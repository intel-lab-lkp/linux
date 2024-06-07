RDS self-tests
==============

Usage:

    # create a suitable .config
    tools/testing/selftests/net/rds/config.sh

    # build the kernel
    make -j128

    # launch the tests in a VM
    tools/testing/selftests/net/rds/run.sh

An HTML coverage report will be output in /tmp/rds_logs/coverage/.
