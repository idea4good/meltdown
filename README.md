# meltdown
In this demo, we will access kernel data(linux_proc_banner) with meltdown bug.

Note: Support Linux x64 only

## How to build & run?
1. `gcc meldown.c`
2. `./run.sh`

If everything is ok, You will get linux_proc_banner: "%s version %s"
