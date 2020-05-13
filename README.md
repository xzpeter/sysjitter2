sysjitter2
==========

Introduction
------------

This is a new sysjitter called sysjitter2, which is based on the old sysjitter
1.3 code base, however it's totally different thing.

Majorly, we want to address these issues:

  - Poll based test model, covers the whole test duration of jitters (e.g.,
    cyclictest won't be able to detect jitter if happened during idle).

  - Simple enough: unlike more complicated test cases like queuelet and
    accumulative effect of jitter delays, this is the baseline tool.

  - Suport cpu-list and multi-thread: major reason I hacked on sysjitter not
    jitterz because it's easier...

  - Record max latency/jitter for each testing core.

  - No dropping of samples: Use 64bit buckets as counter of jitters.
    Configurable buckets, default is 1us, 2us, 3us, ..., 31us, >=32us.  64bit
    counter means even if one jitter per TSC increase it'll take hundreds of
    years to overflow - so it means no worry on overflow too.

  - Very small memory footprint: unlike sysjitter who records every
    interruption.  It also means this test can run forever with constant memory
    footprint.

  - A few other good stuff to have (fifo support; ftrace support borrowed from
    rt-tests; interact with sigterm and sigint so we can interrupt the test
    anytime we want without losing data, etc. or anything more we think
    sensible - I'd be glad to implement it)

Sample output
-------------

    [root@localhost ~]# ./sysjitter2 --cpu-list 2,3,4,5,6,7,8,9 --rtprio 1 --runtime 1
    Version: 2.01
    
          core_i:    2 3 4 5 6 7 8 9
         cpu_mhz:    2197 2197 2197 2196 2197 2197 2197 2197
        001 (us):    26110432 26110434 26110375 26110373 26110430 26110432 26110371 26110369
        002 (us):    0 0 0 0 0 0 0 0
        003 (us):    0 0 1 1 0 0 1 1
        004 (us):    0 0 0 0 0 0 0 0
        005 (us):    0 0 0 0 0 0 0 0
        006 (us):    0 0 0 0 0 0 0 0
        007 (us):    0 0 0 0 0 0 0 0
        008 (us):    0 0 0 0 0 0 0 0
        009 (us):    0 0 0 0 0 0 0 0
        010 (us):    0 0 0 0 0 0 0 0
        011 (us):    0 0 0 0 0 0 0 0
        012 (us):    0 0 0 0 0 0 0 0
        013 (us):    0 0 0 0 0 0 0 0
        014 (us):    0 0 0 0 0 0 0 0
        015 (us):    0 0 0 0 0 0 0 0
        016 (us):    0 0 0 0 0 0 0 0
        017 (us):    0 0 0 0 0 0 0 0
        018 (us):    0 0 0 0 0 0 0 0
        019 (us):    0 0 0 0 0 0 0 0
        020 (us):    0 0 0 0 0 0 0 0
        021 (us):    0 0 0 0 0 0 0 0
        022 (us):    0 0 0 0 0 0 0 0
        023 (us):    0 0 0 0 0 0 0 0
        024 (us):    0 0 0 0 0 0 0 0
        025 (us):    0 0 0 0 0 0 0 0
        026 (us):    0 0 0 0 0 0 0 0
        027 (us):    0 0 0 0 0 0 0 0
        028 (us):    0 0 0 0 0 0 0 0
        029 (us):    0 0 0 0 0 0 0 0
        030 (us):    0 0 0 0 0 0 0 0
        031 (us):    0 0 0 0 0 0 0 0
        032 (us):    0 0 0 0 0 0 0 0
          maxlat:    1 1 3 3 1 1 3 3 (us)
         runtime:    0.998 0.998 0.998 0.999 0.998 0.998 0.998 0.998 (sec)
