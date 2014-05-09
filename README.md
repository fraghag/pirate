# Cache Pirate

By: [Ragnar Hagg](mailto:ragnar.hagg@gmail.com)

This implementation of a Cache Pirate was done for a project master course in Computational Science at the UART group at Uppsala Univeristy and was finished in February 2014.

Based on a [earlier](https://github.com/andysan/perf_tools) implementation of the Cache Pirate by Andreas Sandberg.

## Usage


### Installation


**Make sure these two packages are installed:**

- [libpfm4](http://perfmon2.sourceforge.net/)
- [Google Protobuf](https://developers.google.com/protocol-buffers/)


**Download the Pirate from my git repository and compile:**

`git clone git@github.com:fraghag/pirate.git pirate`

`cd pirate`

`make`

**Activate huge pages in your Linux system:**

Five-step process found [here](https://wiki.debian.org/Hugepages).

### Running the Pirate


The Pirate is run through the Linux terminal command with:

`./perfpirate [arguments] -- [target_command target_arguments]`

#### Example

`./perfpirate -c 0 -C 1 --sample-period=100000 -e BRANCH\_INSTRUCTIONS\_RETIRED -e MISPREDICTED\_BRANCH\_RETIRED -o result.pb -- my\_target -s 4096`

#### Arguments

`-c, --target-cpu=CPU`
Pin target process to CPU, default is 0.

`-C, --pirate-cpu=CPU`
Pin pirate to CPU. Repeat this option for more pirates several Pirate threads. It is recommended that you set this by yourself, since a working default depends on the hardware.

`-o, --output=FILE`
Filename and path of Protobuf output file. Default is `perfpirate.pb`.

`-e, --target-event=EVENT`
Events to measure on the target. EVENT given with the name used in *libpfm4*.

`-E, --pirate-event=EVENT`
Events to measure on the Pirate. EVENT given with the name used in *libpfm4*.

`-r, --target-raw-event=EVENT`
Raw events to measure on the target. EVENT given in the form of a string beginning with '`raw:`' and then the raw event mask (if hexadecimal mask start with '`raw:0x`.

`-s, --pirate-size=SIZE`
Pirate data set size. This disables the online size adjustment, and just samples the given SIZE.

`-h, --target-heat-time=TIME` 
Time in microseconds for target to heat between sample cycles. Default is 10,000.

`--sample-freq=N`
Set event sample frequency for the instruction counter on the target. Do not use together with the \`--sample-period} argument.

`--sample-period=N`
Set event sample period for the instruction counter on the target. Default value is 1,000,000. Do not use together with the \`--sample-freq} argument.

`-?, --help`
Gives a help list.

`--usage`
Give a short usage message.


### Performance counters


With the **libpfm-4.4.0** package there is a application `examples/showevtinfo` which shows all the available events for the current architecture and OS with their libpfm4 names and available unit-masks. It also shows the counters' code which can be used to find the counter in the Intel developer manual where they are documented in the *PERFORMANCE-MONITORING EVENTS* chapter. To use unit-masks with a counter just add them after the counter name separated with colons: `PFM4_EVENT_NAME:UMASK1:UMASK2`


### Common pitfalls


#### Cache architecture

If the cache has non-uniform access times the target and the Pirate could have different access times to different parts of the cache and the same expected result as above might not be applicable. It is hard to say what the result should be if this is causing problems.

#### LLC bandwidth contention

If the combined traffic to the LLC gets too big the applications on all cores will start having to wait for LLC accesses and memory fetches. This is probably the case when the Pirate's CPI increases without an increase in miss ratio. This is more likely to happen when running several Pirate threads, and can also happen if the target has a high memory access rate.

#### Pin processes to the right cores

If the Pirate's CPI does not increase a lot when the target's cache size goes toward zero then make sure that the Pirate is pinned to a core that share LLC with the target. The risk of this is high if your computer has more than one CPU, and you pin the Pirate and target to different CPUs.
Also be sure that the Pirate and target are not pinned to the same core. The risk of this is high if each core has several threads. On Linux systems, different threads are usually named `cpuX` where `X` is the thread id number.
In the folder `/sys/devices/system/cpu/cpuX/cache` where `X` is the thread id number you can find out which caches are shared with which threads. If threads share LLC cache they are on the same CPU, and if they share L1 cache they threads on the same core.

#### DVFS (Dynamic Volt and Frequency Scaling

If the Pirate's CPI suddenly decreases when the target's cache size gets smaller the OS might have used DVFS to clock down the core that the Pirate is running on. If you suspect that this might be the case you can disable DVFS in Linux or the BIOS.

#### Unpredictable performance counters

If you get very unexpected results, like a 200% fetch ratio, then the counters you are using do probably not measure what you expect. A counter for LLC read misses for example might count the LLC read misses from all cores on that CPU even though the counter is pinned to a specific core. Check the documentation on the counter used, and see if there are more suitable counters. 

#### Caches that are not inclusive
If the target seems to have access to more cache that it should have, then the cache might be *non-inclusive* or *exclusive*. If the cache is *exclusive* there will be no copy in the L3 cache of the data kept in the L1 and L2. Therefore the size of the available cache to an application is the sum of the L1, L2 and L3 cache size. The Pirate will also be stealing less of the shared cache since part of its dataset will reside in the L1 and L2 cache. If the cache is *non-inclusive* it is harder to say, but the result should be correct as long as the Pirate's dataset doesn't fit in the L2 cache.

#### OS using cache

If the Pirate seems to steal more cache than it is supposed to, then it might be the OS that is using some of the cache. You can test this by running the random access microbenchmark with a dataset equal to the cache size, and without the Pirate stealing any cache. The percent miss ratio for the benchmark will also be the ratio of the cache that is missing.

## Acknowledgments

I would like to thank the UART team at Uppsala University for involving me in their work and giving me this opportunity to do this project, and Maya Neytcheva for supervising the project course. The biggest thanks goes to my supervisor Andreas Sandberg who has had the patience to help and teach me so much.
