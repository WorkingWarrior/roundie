"""Userspace USB Bulk-Only client for the MSC probe: behaves like a host that reads
the volume, adds a file with mtools, writes the changed sectors and ejects."""
import struct, subprocess, sys, time, os
import usb.core, usb.util

SCRATCH = os.path.dirname(os.path.abspath(__file__))
dev = usb.core.find(idVendor=0x303A, idProduct=0x4003)
if dev is None:
    sys.exit("probe not found")
cfg = dev.get_active_configuration()
intf = [i for i in cfg if i.bInterfaceClass == 8][0]
if dev.is_kernel_driver_active(intf.bInterfaceNumber):
    dev.detach_kernel_driver(intf.bInterfaceNumber)
usb.util.claim_interface(dev, intf.bInterfaceNumber)
ep_in = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)
ep_out = usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)
tag = 0

def scsi(cb, length=0, write=None, timeout=5000):
    global tag
    tag += 1
    cb16 = bytes(cb) + bytes(16 - len(cb))
    flags = 0x00 if write is not None else 0x80
    cbw = struct.pack("<IIIBBB", 0x43425355, tag, length, flags, 0, len(cb)) + cb16
    ep_out.write(cbw, timeout)
    data = b""
    if write is not None:
        ep_out.write(write, timeout)
    elif length:
        while len(data) < length:
            data += bytes(ep_in.read(min(length - len(data), 4096), timeout))
    csw = bytes(ep_in.read(13, timeout))
    sig, rtag, residue, status = struct.unpack("<IIIB", csw)
    assert sig == 0x53425355 and rtag == tag, "bad CSW"
    return status, data

def request_sense():
    st, d = scsi([0x03, 0, 0, 0, 18, 0], 18)
    return d[2] & 0xF, d[12], d[13]

st, _ = scsi([0x00, 0, 0, 0, 0, 0])
print("TEST UNIT READY status", st, "(0 = ready)", "sense" if st else "", request_sense() if st else "")
st, inq = scsi([0x12, 0, 0, 0, 36, 0], 36)
print("INQUIRY", inq[8:16].decode(errors="replace").strip(), inq[16:32].decode(errors="replace").strip())
st, cap = scsi([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8)
last_lba, block = struct.unpack(">II", cap)
blocks = last_lba + 1
print("capacity", blocks, "blocks of", block, "bytes =", blocks * block // 1024, "KiB")

if "--eject-only" in sys.argv:
    st, _ = scsi([0x1B, 0, 0, 0, 0x02, 0])
    print("START STOP UNIT (eject) status", st)
    time.sleep(0.5)
    st, _ = scsi([0x00, 0, 0, 0, 0, 0])
    print("TEST UNIT READY after eject:", st, request_sense() if st else "")
    sys.exit(0)
image = os.path.join(SCRATCH, "msc_image.bin")
t0 = time.time()
chunk = 64
data = bytearray()
for lba in range(0, blocks, chunk):
    n = min(chunk, blocks - lba)
    st, d = scsi([0x28, 0, *struct.pack(">I", lba), 0, *struct.pack(">H", n), 0], n * block)
    assert st == 0, "read failed"
    data += d
elapsed = time.time() - t0
print("read %d KiB in %.1f s = %.0f KiB/s" % (len(data) // 1024, elapsed, len(data) / 1024 / elapsed))
open(image, "wb").write(data)
before = bytes(data)
print(subprocess.run(["file", "-s", image], capture_output=True, text=True).stdout.strip())
part_start, part_size = struct.unpack("<II", data[446 + 8:446 + 16])
part_type = data[446 + 4]
print("partition 1: type 0x%02x start sector %d size %d sectors" % (part_type, part_start, part_size))
fat = image + "@@" + str(part_start * block)
print(subprocess.run(["mdir", "-i", fat, "-/", "::"], capture_output=True, text=True).stdout)

# a 120 KiB package-sized test file, as a host would copy it
payload = os.path.join(SCRATCH, "test_package.rpk")
open(payload, "wb").write(bytes((i * 7) % 251 for i in range(120 * 1024)))
r = subprocess.run(["mcopy", "-i", fat, payload, "::/Inbox/TEST.RPK"], capture_output=True, text=True)
print("mcopy rc", r.returncode, r.stderr.strip())
after = open(image, "rb").read()
changed = [lba for lba in range(blocks) if before[lba*block:(lba+1)*block] != after[lba*block:(lba+1)*block]]
print("changed sectors:", len(changed))

t0 = time.time()
i = 0
while i < len(changed):
    j = i
    while j + 1 < len(changed) and changed[j + 1] == changed[j] + 1 and j - i < 63:
        j += 1
    lba, n = changed[i], j - i + 1
    st, _ = scsi([0x2A, 0, *struct.pack(">I", lba), 0, *struct.pack(">H", n), 0], n * block, write=after[lba*block:(lba+n)*block])
    assert st == 0, "write failed at %d" % lba
    i = j + 1
elapsed = time.time() - t0
print("wrote %d sectors in %.2f s" % (len(changed), elapsed))
st, _ = scsi([0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0])
print("SYNCHRONIZE CACHE status", st)
# read back to verify
mism = 0
for lba in changed[:200]:
    st, d = scsi([0x28, 0, *struct.pack(">I", lba), 0, 0, 1, 0], block)
    if d != after[lba*block:(lba+1)*block]:
        mism += 1
print("read-back mismatches (first 200 changed sectors):", mism)
if "--eject" in sys.argv:
    st, _ = scsi([0x1B, 0, 0, 0, 0x02, 0])
    print("START STOP UNIT (eject) status", st)
    time.sleep(0.5)
    st, _ = scsi([0x00, 0, 0, 0, 0, 0])
    print("TEST UNIT READY after eject:", st, request_sense() if st else "")
usb.util.release_interface(dev, intf.bInterfaceNumber)
