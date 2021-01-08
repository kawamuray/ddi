#!/bin/bash
set -e

root_dir=$(dirname $0)
cd $root_dir

function show_help() {
    cat <<EOS >&2
Usage:
  $0 [OPTIONS] create [MOUNTPOINT_PATH] [DEV_NAME] - Create a new DDI device mounted at MOUNTPOINT_PATH with optional name DEV_NAME
  $0 delete MOUNTPOINT_PATH|DEV_PATH               - Delete a previously setup DDI device indicated either by mountpoint or by its path
  $0 clean                                         - Cleanup kernel module loaded by this program
Options:
  -h - Show this help
  -u - Skip mounting created device and print its path instead
  -d - The target device instead of mapping a file as a loopback device
  -m - The file path to use as a virtual device by mapping it to a loopback device (default: a file under /tmp)
  -s - Size to allocate for the virtual (file-based) device, in KiB (default: 100240 (100MiB))
  -t - Filesystem to provision for the target device. This program calls mkfs.\$ARGS with no argument (default: xfs)
EOS
}


function module_loaded() {
    grep -q '^dm_ddi ' /proc/modules
    return $?
}

skip_mount=0
dev_path=""
vdisk_path=""
vdisk_size=100240
fs_type="xfs"

while getopts "hud:m:s:t:" opt; do
    case "$opt" in
        h)  show_help
            exit 0
            ;;
        u)  skip_mount=1
            ;;
        d)  dev_path=$OPTARG
            ;;
        m)  vdisk_path=$OPTARG
            ;;
        s)  vdisk_size=$OPTARG
            ;;
        t)  fs_type=$OPTARG
            ;;
        \?)  show_help
             exit 1
             ;;
    esac
done

shift $((OPTIND-1))
subcmd="$1"
mountpoint="$2"
dev_name="$3"

function do_create() {
    if [ $skip_mount = 0 ] && [ -z "$mountpoint" ]; then
        echo "Error: MOUNTPOINT_PATH argument is required unless you use -u option" >&2
        show_help
        return 1
    fi
    if [ -z "$dev_name" ]; then
        i=1
        while true; do
            dev_name="ddi-$i"
            if [ ! -e "/dev/mapper/$dev_name" ]; then
                break
            fi
            i=$(($i + 1))
        done
    fi

    if ! module_loaded; then
        echo "Building and loading dm-ddi module" >&2
        make
        insmod dm-ddi.ko
    fi

    if [ -z "$dev_path" ]; then
        mkfs=0
        if [ -z "$vdisk_path" ]; then
            vdisk_path=$(mktemp /tmp/ddi-vdisk-XXXXX)
            echo "Creating a virtual disk as a file: $vdisk_path" >&2
            dd if=/dev/zero of="$vdisk_path" bs=1024 count="$vdisk_size" >/dev/null
            mkfs=1
        fi

        echo "Creating a loopback device for vdisk: $vdisk_path" >&2
        lo_dev=$(/sbin/losetup --show --find "$vdisk_path")
        echo "Loopback device $lo_dev allocated" >&2

        if [ $mkfs = 1 ]; then
            echo "Creating filesystem $fs_type at $lo_dev" >&2
            /sbin/mkfs.$fs_type $lo_dev
        fi

        dev_path="$lo_dev"
    fi

    echo "Setting up dm-ddi for $lo_dev" >&2
    echo "0 `/sbin/blockdev --getsz $lo_dev` ddi $lo_dev 0 0 $lo_dev 0 0" | /sbin/dmsetup create "$dev_name"

    if [ $skip_mount = 0 ]; then
        echo "Mounting $lo_dev to $mountpoint" >&2
        mount "/dev/mapper/$dev_name" "$mountpoint"
        echo "Device is ready at $mountpoint" >&2
    else
        echo "Device is ready: /dev/mapper/$dev_name" >&2
    fi

}

function do_delete() {
    if [[ "$mountpoint" = "/dev/mapper/"* ]]; then
        dev_path="$mountpoint"
    elif [ -n "$mountpoint" ]; then
        dev_path=$(grep "$mountpoint" /proc/mounts | cut -f1 -d' ')
        echo "Unmounting $mountpoint" >&2
        umount "$mountpoint"
    else
        echo "Error: MOUNTPOINT_PATH or DEV_PATH is required" >&2
        show_help
        return 1
    fi

    dev_name="$(echo $dev_path | grep -oE '[^/]+$')"

    lo_dev=$(/sbin/dmsetup ls --tree -o 'blkdevname' | grep -A1 "^$dev_name " | tail -1 | grep -oE 'loop[0-9]+')
    echo "Removing dm device $dev_path" >&2
    /sbin/dmsetup remove "$dev_path"

    if [ -n "$lo_dev" ]; then
        echo "Detatching loopback device $lo_dev" >&2
        /sbin/losetup -d "/dev/$lo_dev"
    fi
}

function do_clean() {
    if module_loaded; then
        echo "Unloading dm-ddi module" >&2
        rmmod dm-ddi.ko
    fi
}

case "$subcmd" in
    create)
        do_create
        ;;
    delete)
        do_delete
        ;;
    clean)
        do_clean
        ;;
    *)
        if [ -n "$subcmd" ]; then
            echo "Error: no such subcommand: $subcommand" >&2
        fi
        show_help
        exit 1
esac
