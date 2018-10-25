# lockscale - test program for scaling of file-based locks

    lockscale [options] lock-algo
	
**lock-algo** is the algorithm used for locking; either `flock`, `fcntl`, or
`sysv`.

 * `fcntl`: workers use `fcntl(..., F_OFD_SETLKW, ...)`,
 * `flock`: workers use `flock(..., LOCK_EX)`,
 * `sysv`: workers use SysV semaphores.
 
## Options

 * -i <us>: "idle" time in us: the time a worker will sleep after releasing
   the lock
 * -b <us>: "busy" time in us: the time a worker will sleep while holding the
   lock. This value may be negative, in which case the workers will do an
   actual busy-wait for the absolute value oft he number rather then sleeping.
 * -t <secs>: runtime for the test, default is 10
 * -w <workers>: colon-separated list of number of worker processes contending
   for the lock. e.g. `-w 10:100:1000` means the test will be run with 10
   workers first, then 100, then 1000.
   
## Output
 
 The output is formatted in a way that can be easily parsed by [gnuplot][1].
 
    # flock 1000 10 120
    10 1.200e+02 9.600e-01 9.830e+00 104788 10471 10499 10478.8 8.2
    20 1.200e+02 1.180e+00 9.650e+00 105030 5236 5273 5251.5 11.2
    50 1.200e+02 1.060e+00 9.940e+00 104876 2078 2116 2097.5 9.7
    100 1.200e+02 9.900e-01 1.124e+01 104797 1018 1074 1048.0 11.3
    200 1.200e+02 1.000e+00 1.213e+01 104924 503 539 524.6 6.8
    500 1.201e+02 7.700e-01 5.995e+01 106345 177 247 212.7 12.6
    1000 1.202e+02 1.030e+00 1.665e+02 107432 79 139 107.4 9.9

The first line prints the lock algorithm, "busy" value in ms, "idle" value in
ms, and duration. The following lines contain measured values in columns:

 1. number of workers
 2. measured test duration
 3. CPU time (user)
 4. CPU time (system)
 5. total number of lock acquisitions
 6. minimum number of lock acquisitions over all workers
 7. maximum number of lock acquisitions over all workers
 8. average number of lock acquisitions
 9. standard deviation of number of lock acquisitions
 
## Theory of operation
 
The program starts the given number of worker processes. The workers contend
for a file lock of a regular file, usually on tmpfs, using the given locking
algorithm. Each worker acquires the lock, holds it for "busy" microseconds,
releases it, sleeps for "idle" microseconds, and starts over again, until it's
interrupted by a signal. The main process kills the workers when the given
test duration is over.

The maximum reachable number of lock acquisitions is 1000000*duration/busy. In
theory, it should not depend on the number of contenders, as long as a
sufficient number of contenders keeps the lock held (busy) all the time.
The main metric for how well the locking algorithm scales is the total number
of lock acquisitions as function of number of contenders. The CPU time gives
an idea whether busy waiting occurs (e.g. in spinlocks in the kernel). The
last 4 columns tell whether the algorithm is "fair". The standard deviation
should be small compared to the average, and minimum and maximum number of
lock acquisitions should be close to each other.

### No byte ranges!

The program tests only basic locking of entire files. Actually, the file
being locked is a 0-byte file. No testing of separate or overlapping byte
ranges is performed.

### "sysv" locking algorithm

This locking algorithm is not a real per-file lock.
Instead it uses a semaphore set with the largest possible power-of-2 
number of semaphores supported by the system (typically 16384), and a 
hash function to map a file name to a semaphore index. Thus it can be expected
to work *almost* like a per-file lock as long as no more than ~1000
files are being locked (beyond that, hash collisions would become 
likely - I haven't done the math). For a single file, as in this test,
it's perfectly usable. Of course, byte ranges are not supported. 
It serves mostly as a reference for the other algorithms, because the
semaphore implementation in the kernel is very well optimized. Indeed,
I haven't observed any scaling issues with `sysv` with this program.

## Building

The program needs to be compiled and liked with [spookyhash][2]. A sample
compile command line would look like this:

    cc -Wall -Wpedantic -I$HOME/spookyhash/src -g -O2 \
	   -o lockscale lockscale.c \
	   $HOME/spookyhash/build/bin/Release/libspookyhash.a -lm

[1]: http://www.gnuplot.info/
[2]: https://github.com/centaurean/spookyhash
