"""Pack the app into a webOS .ipk and write the Homebrew Channel manifest next to it.

An .ipk is an `ar` archive of debian-binary, control.tar.gz and data.tar.gz, with the app under
usr/palm/applications/<id>/. Written here instead of with ares-package so the build needs no
Node.js. Timestamps and owners are fixed, so the same inputs give the same sha256.

usage: make_ipk.py --binary build/launcher --sdl <libSDL2-2.0.so.0> --package package --out build
                   --repo https://github.com/<owner>/<repo> [--homehook build/homehook]

--homehook adds the root-only Home key hook (homehook/): ezinject, libmytvhook.so, watch.sh.
"""
import argparse
import gzip
import hashlib
import io
import json
import os
import tarfile

MTIME = 1700000000


def tar_gz(entries):
    """entries: (archive path, bytes or None for a directory, mode)."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.GNU_FORMAT) as tar:
        for path, data, mode in entries:
            info = tarfile.TarInfo(path)
            info.mtime, info.uid, info.gid, info.uname, info.gname = MTIME, 0, 0, "root", "root"
            info.mode = mode
            if data is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode="wb", mtime=MTIME) as gz:
        gz.write(raw.getvalue())
    return out.getvalue()


def ar(members):
    out = [b"!<arch>\n"]
    for name, data in members:
        out.append(f"{name:<16}{MTIME:<12}{0:<6}{0:<6}{0o100644:<8o}{len(data):<10}`\n".encode())
        out.append(data + (b"\n" if len(data) % 2 else b""))
    return b"".join(out)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True)
    p.add_argument("--sdl", required=True)
    p.add_argument("--package", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--repo", required=True)
    p.add_argument("--homehook")
    a = p.parse_args()

    info = json.load(open(os.path.join(a.package, "appinfo.json")))
    app_id, version = info["id"], info["version"]
    root = f"usr/palm/applications/{app_id}"

    def read(path):
        with open(path, "rb") as f:
            return f.read()

    files = [("appinfo.json", read(os.path.join(a.package, "appinfo.json")), 0o644),
             ("launcher", read(a.binary), 0o755),
             ("lib/libSDL2-2.0.so.0", read(a.sdl), 0o755),       # read() follows the symlink
             ("icon.png", read(os.path.join(a.package, "icon.png")), 0o644),
             ("largeIcon.png", read(os.path.join(a.package, "largeIcon.png")), 0o644),
             ("splash.png", read(os.path.join(a.package, "splash.png")), 0o644)]
    wp_dir = os.path.join(a.package, "wallpapers")
    files += [(f"wallpapers/{n}", read(os.path.join(wp_dir, n)), 0o644)
              for n in sorted(os.listdir(wp_dir)) if n.endswith(".png")]

    dirs = ["usr", "usr/palm", "usr/palm/applications", root, root + "/lib", root + "/wallpapers"]
    if a.homehook:
        files += [("homehook/ezinject", read(os.path.join(a.homehook, "ezinject.bin")), 0o755),
                  ("homehook/libmytvhook.so", read(os.path.join(a.homehook, "libmytvhook.so")), 0o755),
                  ("homehook/watch.sh", read("homehook/watch.sh"), 0o755)]
        dirs.append(root + "/homehook")
    # ares-package writes this too. webosbrew's ipk-verify refuses a package without it.
    pkg_dir = f"usr/palm/packages/{app_id}"
    package_info = json.dumps({"app": app_id, "id": app_id, "loc_name": info["title"],
                               "package_format_version": 2, "vendor": info["vendor"],
                               "version": version}, indent=4).encode()
    dirs += ["usr/palm/packages", pkg_dir]
    data = tar_gz([(d, None, 0o755) for d in dirs] + [(f"{root}/{n}", b, m) for n, b, m in files]
                  + [(f"{pkg_dir}/packageinfo.json", package_info, 0o644)])
    installed_kb = sum(len(b) for _, b, _ in files) // 1024
    control = (f"Package: {app_id}\nVersion: {version}\nSection: misc\nPriority: optional\n"
               f"Architecture: arm\nInstalled-Size: {installed_kb}\n"
               f"Maintainer: {info['vendor']}\nDescription: {info['title']}\n"
               f"webOS-Package-Format-Version: 2\nwebOS-Packager-Version: x.y.x\n").encode()
    ipk = ar([("debian-binary", b"2.0\n"),
              ("control.tar.gz", tar_gz([("control", control, 0o644)])),
              ("data.tar.gz", data)])

    ipk_name = f"{app_id}_{version}_arm.ipk"
    with open(os.path.join(a.out, ipk_name), "wb") as f:
        f.write(ipk)
    raw = a.repo.replace("https://github.com/", "https://raw.githubusercontent.com/")
    manifest = {
        "id": app_id,
        "version": version,
        "type": "native",
        "title": info["title"],
        "appDescription": "A fast, native tvOS-style home screen for LG webOS TVs, with no ads",
        "iconUri": f"{raw}/main/package/icon160.png",
        "sourceUrl": a.repo,
        "rootRequired": False,
        "ipkUrl": ipk_name,                       # relative: resolved against the manifest's URL
        "ipkHash": {"sha256": hashlib.sha256(ipk).hexdigest()},
        "ipkSize": len(ipk),
        "installedSize": installed_kb,
    }
    with open(os.path.join(a.out, f"{app_id}.manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    print(f"{ipk_name}: {len(ipk)} bytes, sha256 {manifest['ipkHash']['sha256']}")


if __name__ == "__main__":
    main()
