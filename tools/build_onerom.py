# PlatformIO pre-script: builds onerom-ffi (One ROM's image builder and
# firmware parser, Rust) for the ESP32-P4 and links it in. Needs rustup with
# the riscv32imafc-unknown-none-elf target (see onerom-ffi/README.md).
import os
import subprocess

Import("env")

root = env.subst("$PROJECT_DIR")
crate = os.path.join(root, "onerom-ffi")
target = "riscv32imafc-unknown-none-elf"
cargo = os.path.expanduser("~/.cargo/bin/cargo")
if not os.path.exists(cargo):
    cargo = "cargo"

print("onerom-ffi: cargo build --release --target " + target)
subprocess.check_call([cargo, "build", "--release", "--target", target], cwd=crate)

# Rust's compiler_builtins carries C helpers (__bswapsi2 and the like) built
# soft-float, which ld refuses to mix with ESP-IDF's single-float objects.
# libgcc has every one of them, so a copy of the archive without them is linked.
import shutil
tools = os.path.expanduser("~/.platformio/packages/toolchain-riscv32-esp/bin")
src_a = os.path.join(crate, "target", target, "release", "libonerom_ffi.a")
out_dir = os.path.join(env.subst("$BUILD_DIR"), "onerom")
os.makedirs(out_dir, exist_ok=True)
out_a = os.path.join(out_dir, "libonerom_ffi.a")
if not os.path.exists(out_a) or os.path.getmtime(out_a) < os.path.getmtime(src_a):
    shutil.copyfile(src_a, out_a)
    info = subprocess.run([os.path.join(tools, "riscv32-esp-elf-readelf"), "-h", out_a],
                          capture_output=True, text=True).stdout
    soft, member = [], None
    for line in info.splitlines():
        if line.startswith("File: ") and "(" in line:
            member = line[line.rindex("(") + 1:-1]
        elif "Flags:" in line and "soft-float" in line and member:
            soft.append(member)
    if soft:
        subprocess.check_call([os.path.join(tools, "riscv32-esp-elf-ar"), "d", out_a] + soft)
    print("onerom-ffi: dropped %d soft-float helper objects" % len(soft))

env.Append(LIBPATH=[out_dir])
env.Append(LIBS=["onerom_ffi"])
