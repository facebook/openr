#!/usr/bin/env python

import ipaddress

batch_num = 10
batch_size = 100
starting_prefix = u'110.1.1.0'
prefix_len =24

def increment_ipv4_prefix(prefix_string, offset):
    prefix = int(ipaddress.ip_address(prefix_string))
    offset = 1 << (32 - prefix_len)
    return prefix+offset

prefix = starting_prefix
prefix_str_list = str(starting_prefix) + "/" + str(prefix_len)

for batch_index in range(0,batch_num):
    for route_index in range(0,batch_size):
        prefix = increment_ipv4_prefix(prefix, prefix_len)
        prefix_str_list = prefix_str_list+ "," + str(ipaddress.ip_address(prefix)) + "/" + str(prefix_len)



print prefix_str_list
