ddi - Disk Delay Injection
==========================

Inject arbitrary delay at requesting I/O for block devices, so we can simulate slowing down disks quite realistically, even from the kernel's perspective, like when kernel task writing back dirty pages.

This project provides quite similar functionality to, and is based on [dm-delay in kernel upstream](https://www.kernel.org/doc/html/latest/admin-guide/device-mapper/delay.html) except that this project allows you to setup a mountpointa with delay injected device in single command, and also let you controlling injected latency dynamically without needing to remount the target volume.

# Motivation

`dm-delay` is sufficient to setup a simple environment for testing disk latency impact for applications. However, realistically, an unexpected disk latency happens in very short duration while the application had already start up and running. To let applications startup normally, create necessary states on disks and then inject delay to see what happens, we need to control disk latency dynamically after creating and mounting the device, not in time prior to mounting the device.

Also, setting up a slow disk with `dm-delay` involves multiples steps that I have to look into a note every time I do it. I wanted to make it easy enough to quickly do when I need it.

A [Stack Exchange answer](https://serverfault.com/questions/523509/linux-how-to-simulate-hard-disk-latency-i-want-to-increase-iowait-value-withou) well explains what you need to do to set it up manually.

# Usage

Setup a delay injected device and mount it

```sh
# Simplest setup
$ sudo ./ddi-setup.sh create /path/to/mount

# Name the device (creates /dev/mapper/delayed)
$ sudo ./ddi-setup.sh create /path/to/mount delayed

# Setup device but skip mounting
$ sudo ./ddi-setup.sh -u create

# Custom disk image size
$ sudo ./ddi-setup.sh -s 2097152 create /path/to/mount

# Map existing file as the disk image instead of creating one
$ sudo ./ddi-setup.sh -m ./my-disk-img create /path/to/mount

# Use an actual device instead of pseudo, file-based disk image
$ sudo ./ddi-setup.sh -d /dev/sda create /path/to/mount
```

Once the device is created, you can use sysfs to control read/write delay dynamically

```sh
$ ls -l /sys/fs/ddi/7:0/
total 0
-rw-rw-rw- 1 root root 4096 Jan  8 20:06 read_delay
-rw-rw-rw- 1 root root 4096 Jan  8 20:06 write_delay

# Set 1000ms write delay
$ echo 1000 | sudo tee /sys/fs/ddi/7:0/write_delay
1000

# Read current read delay
$ cat /sys/fs/ddi/7:0/read_delay
10
```

Delete a delay injected device

```sh
# Unmount and delete the associated device
$ sudo ./ddi-setup.sh delete /path/to/mount

# Delete the device (prior unmount required)
$ sudo ./ddi-setup.sh delete /dev/mapper/my-device
```

Cleanup kernel module

```sh
$ sudo ./ddi-setup.sh clean
```

# Demo

```sh
# Just a regular disk volume, fast enough

$ time sudo dd if=/dev/zero of=/slow-volume/chunk bs=1024 count=$((1024 * 1024)) conv=fsync
real    0m2.559s
user    0m0.122s
sys     0m2.408s

# Setup delay injected device and mount it onto /slow-volume

$ sudo ./ddi-setup.sh -s 2097152 create /slow-volume
Creating a virtual disk as a file: /tmp/ddi-vdisk-exfub
2097152+0 records in
2097152+0 records out
2147483648 bytes (2.1 GB) copied, 6.65042 s, 323 MB/s
Creating a loopback device for vdisk: /tmp/ddi-vdisk-exfub
Loopback device /dev/loop0 allocated
Creating filesystem xfs at /dev/loop0
...
Setting up dm-ddi for /dev/loop0
Mounting /dev/loop0 to /slow-volume
Device is ready at /slow-volume

# Device Mapper device created

$ ls -l /dev/mapper/ddi-1
lrwxrwxrwx 1 root root 7 Jan  8 20:05 /dev/mapper/ddi-1 -> ../dm-0

# Set write delay to 1000ms (1sec)

$ echo 1000 | sudo tee /sys/fs/ddi/7:0/write_delay
1000

# Now it takes 7 seconds to complete

$ time sudo dd if=/dev/zero of=/slow-volume/chunk bs=1024 count=$((1024 * 1024)) conv=fsync
real    0m7.557s
user    0m0.139s
sys     0m3.198s


# Set write delay to 8000ms (8secs)

$ echo 8000 | sudo tee /sys/fs/ddi/7:0/write_delay
8000

# Now it takes nearly 1 minute to complete

$ time sudo dd if=/dev/zero of=/slow-volume/chunk bs=1024 count=$((1024 * 1024)) conv=fsync
real    0m54.688s
user    0m0.108s
sys     0m3.347s


# Works for read as well

$ echo 1 | sudo tee /proc/sys/vm/drop_caches; time sudo dd if=/slow-volume/chunk of=/dev/null bs=4096
real    0m0.824s
user    0m0.029s
sys     0m0.598s

$ echo 10 | sudo tee /sys/fs/ddi//7:0/read_delay
10
$ echo 1 | sudo tee /proc/sys/vm/drop_caches; time sudo dd if=/slow-volume/chunk of=/dev/null bs=4096
real    0m41.035s
user    0m0.002s
sys     0m0.604s

# Delete the slow device and put it back to the original state

$ sudo ./ddi-setup.sh delete /slow-volume
Unmounting /slow-volume
Removing dm device /dev/mapper/ddi-1
Detatching loopback device loop0
```

License
=======

GNU General Public License, version 2

See [LICENSE](./LICENSE).
