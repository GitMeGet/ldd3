# Quickstart

1. load scull kernel module
```
$ ./scull_load.sh
```
2. write to scull character device
```
$ echo "hello world" > /dev/scull
```
3. read from scull
```
$ cat /dev/scull
hello world
```
4. unload scull
```
$ ./scull_unload.sh
```
