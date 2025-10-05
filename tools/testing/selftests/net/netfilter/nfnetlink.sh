#!/bin/bash

# If nft_ct is a module and is loaded, remove it to test module auto-loading.
# If removal fails, continue anyway since it won't affect the test result.
rmmod nft_ct 2>/dev/null
./nfnetlink