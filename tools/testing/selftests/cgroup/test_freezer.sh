#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# Test freeze hierarchy state
#
ROOT_PATH=/sys/fs/cgroup
skip_test() {
	echo "$1"
	echo "Test SKIPPED"
	exit 4 # ksft_skip
}

write_freeze() {
	path=$1
	value=$2
	echo $value > $ROOT_PATH/$path/cgroup.freeze
}

get_event_frozen()
{
	path=$1
	return $(cat $ROOT_PATH/$path/cgroup.events | grep frozen | awk '{print $2}')
}

# the test subtree:
#        A
#       / \
#      B   C
#    / | \  \
#    D E F   G
#   /
#  H

PATH_MATRIX=(
	'A'
	'A/B'
	'A/C'
	'A/B/D'
	'A/B/E'
	'A/B/F'
	'A/C/G'
	'A/B/D/H'
)
CGRP_CNT=${#PATH_MATRIX[@]}

# Interface value to set/check
ITF_MATRIX=(
	#value write to freeze         expect event value
	#A  B  C  D  E  F  G  H  |  A  B  C  D  E  F  G  H
	'1  0  0  0  0  0  0  0     1  1  1  1  1  1  1  1'
	'1  1  0  0  0  0  0  0     1  1  1  1  1  1  1  1'
	'1  0  1  0  0  0  0  0     1  1  1  1  1  1  1  1'
	'0  0  0  0  0  0  0  0     0  0  0  0  0  0  0  0'

	'1  1  1  1  0  0  0  0     1  1  1  1  1  1  1  1'
	'1  1  1  0  0  0  0  0     1  1  1  1  1  1  1  1'
	'1  1  0  0  0  0  0  0     1  1  1  1  1  1  1  1'
	'1  0  0  0  0  0  0  0     1  1  1  1  1  1  1  1'

	'0  0  0  1  0  0  0  0     0  0  0  1  0  0  0  1'
	'0  0  0  1  0  0  0  1     0  0  0  1  0  0  0  1'
	'0  0  0  1  0  0  0  0     0  0  0  1  0  0  0  1'
	'0  0  1  1  0  0  0  0     0  0  1  1  0  0  1  1'
	'0  0  0  0  0  0  0  0     0  0  0  0  0  0  0  0'
)

prepare()
{
	for i in "${PATH_MATRIX[@]}" ; do
		path="$ROOT_PATH/$i"
		mkdir $path
	done
}

do_clean()
{
	for(( i=$CGRP_CNT-1; i>=0; i-- ));do
		path="/sys/fs/cgroup/${PATH_MATRIX[i]}"
		rmdir $path
	done
}

run_test()
{
	prepare
	cgrp_cnt="${PATH_MATRIX[@]}"
	for i in "${ITF_MATRIX[@]}" ; do
		args=($i)
		#write freeze
		for(( j=0; j<$CGRP_CNT; j++ ));do
			write_freeze "${PATH_MATRIX[j]}" ${args[j]}
		done
		#event frozen should eq expected value
		for(( j=0; j<$CGRP_CNT; j++ ));do
			get_event_frozen "${PATH_MATRIX[j]}"
			if [ $? -ne ${args[j + $CGRP_CNT]} ];then
				echo "failed: $i"
				do_clean
				exit 1
			fi
		done
	done
	do_clean
}

start_time=$(date +%s%N)
run_test
end_time=$(date +%s%N)
elapsed_time=$((end_time - start_time))
echo "Test PASSED, elapsed_time:$elapsed_time (ns)"
exit 0
