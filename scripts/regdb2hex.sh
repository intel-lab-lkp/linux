#! /usr/bin/env bash

# This script extracts the certificate from regulatory.db.p7s and generate a hex file usable
# by the kernel source code, after that take the result and put it with a patch to the kernel
# source tree under net/wireless/certs
# this will be added to shiped_certs
# to verify that your certificate has been added successfully 
# 1. check inside 
# /proc/keys and you will see a line with asymetric issuer and a sha
# 2. dmesg | grep -i x.509
# cfg80211: Loaded X.509 cert '{issuer}: {sha}'

if [ $# -lt 2 ]; then
    echo "${0} <regulatory.db.p7s> <output-filename>"
    exit 0
fi

trap '[ -e ${2}.x509 ] && rm ${2}.x509' SIGINT SIGTERM SIGQUIT 0

openssl pkcs7 -in ${1} -inform DER -print_certs | openssl x509 -inform PEM -outform DER -out ${2}.x509

hex_crt=$(od -An -v -tx1 < ${2}.x509 | sed -e 's/ /\n/g' | sed -e 's/^[0-9a-f]\+$/\0/;t;d' | sed -e 's/^/0x/;s/$/,/')

echo "/* ${2}'s regdb certificate */" >> ${2}.hex

cnt=0
for h in ${hex_crt}; do
    
    cnt=$(( cnt + 1 ))
    nl=$(( cnt % 8 ))
    if [ "$nl" = "0" ]; then
        echo " $h" >> ${2}.hex
    else
        echo -n " $h" >> ${2}.hex
    fi

done