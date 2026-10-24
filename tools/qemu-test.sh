#!/bin/sh
# Runs inside the vcq-dev image: builds vcq.ko against the image's Ubuntu
# kernel, boots that kernel under QEMU with a busybox initramfs, loads the
# module, runs the tests and the benchmark, unloads it and checks the kernel
# log for warnings. Exits non-zero if anything failed.
set -eu
cd "$(dirname "$0")/.."

KVER=$(ls /lib/modules | sort -V | tail -n 1)
make module KDIR="/lib/modules/$KVER/build"
make user STATIC=1

root=$(mktemp -d)
mkdir -p "$root/bin" "$root/proc" "$root/sys" "$root/dev"
cp /bin/busybox "$root/bin/"
for app in sh mount insmod rmmod poweroff dmesg grep; do
	ln -s busybox "$root/bin/$app"
done
cp driver/vcq.ko build/vcq_test build/vcq_bench "$root/"
cat > "$root/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
rc=0
insmod /vcq.ko || rc=1
/vcq_test || rc=1
/vcq_bench || rc=1
rmmod vcq || rc=1
if dmesg | grep -E 'BUG:|WARNING:|Oops|INFO: task|refcount_t'; then rc=1; fi
echo "VCQ_RESULT rc=$rc"
poweroff -f
EOF
chmod +x "$root/init"
(cd "$root" && find . | cpio -o -H newc --quiet | gzip) > build/initramfs.cpio.gz

accel=""
if [ -w /dev/kvm ]; then accel="-enable-kvm -cpu host"; fi
# shellcheck disable=SC2086
timeout 1200 qemu-system-x86_64 $accel -m 1G -smp 4 -nographic -no-reboot \
	-kernel "/boot/vmlinuz-$KVER" -initrd build/initramfs.cpio.gz \
	-append "console=ttyS0 panic=-1 loglevel=4" | tee build/qemu.log
grep -q "VCQ_RESULT rc=0" build/qemu.log
