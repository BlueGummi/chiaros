# charmos - Compact, Hobbyist And Recreational Monolithic Operating System

<p align="center">
<img src="https://github.com/BlueGummi/charmos/blob/main/charmos.png" width="240">
</p>

## Build

Prerequisites: `nasm` and `xorriso`

```bash
./build.sh

```
## Run

```bash
make run # run this in the build directory
```

## Roadmap 

> The :trophy: means the task is a challenge, the :broom: means something is trivial

### TODO:

- [ ] reorganize filesystem generic operations (vfs)

- [ ] reorganize vfs to integrate with generic disks

- [ ] disk I/O IRQ handlers to avoid blocking for unnecessary periods of time (optimization)

- [ ] we can reserve something like IRQs 200-223 or whatever...

- [ ] free functions for devices and other structs

- [ ] mem alloc failure handling (do not power off the 'puter when OOM)

- [ ] once filesystems are up and running, make a logging function to log to a set file

- [ ] block device sector size scans

- [ ] define magic numbers!!
