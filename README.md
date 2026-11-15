## mountzip

Mount a ZIP file as a read-only file system.

## Prerequisites

Linux kernel 6.12

```
apt-get update
apt-get install linux-headers-$(uname -r)
apt-get install build-essential kmod
```

## Build

```
cd /path/to/mountzip
make
```

## Usage

```
cd /path/to/mountzip
insmod build/mountzip.ko
mount -t mountzip /path/to/file.zip /mount/point
```
