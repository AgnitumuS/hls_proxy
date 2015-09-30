# hls_proxy
Simple proxy based on mongoose, this will attempt to keep all segments in memory until a set expiry time

Main purpose of this is to use memory for speed, and also if a new request comes in for a file that is in the process of downloading the request will be fulfilled by sending the data that has already been downloaded from the main source.

A purge system keeps removing any segment older than X seconds from last use.
