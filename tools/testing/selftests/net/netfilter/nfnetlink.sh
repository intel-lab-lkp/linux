#!/bin/bash

exec unshare -n bash -c '
  nft flush ruleset

  rmmod nft_ct

  ./nfnetlink

  nft flush ruleset
'