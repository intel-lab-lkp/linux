net_netfilter_dir=$(dirname "$(readlink -e "${BASH_SOURCE[0]}")")

source "$net_netfilter_dir/../lib.sh"

checktool (){
	if ! $1 > /dev/null 2>&1; then
		echo "SKIP: Could not $2"
		exit $ksft_skip
	fi
}

nf_queue_wait()
{
	local procfile="/proc/self/net/netfilter/nfnetlink_queue"
	local netns id

	netns="$1"
	id="$2"

	# if this file doesn't exist, nfnetlink_module isn't loaded.
	# rather than loading it ourselves, wait for kernel module autoload
	# completion, nfnetlink should do so automatically because nf_queue
	# helper program, spawned in the background, asked for this functionality.
	test -f "$procfile" &&
		ip netns exec "$netns" cat "$procfile" | grep -q "^ *$id "
}
