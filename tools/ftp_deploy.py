#!/usr/bin/env python3
"""Copy a built Omni360.xex to the console over Omni360's own FTP server.

Uploads beside the target as .new, checks the size, then swaps it in, so a
dropped connection never leaves a half-written XEX. Works while the app runs;
restart it afterwards (B on Home, then Exit, when the dashboard path points
at the development folder).
"""

import argparse
import ftplib
import os
import sys


def deploy(host, user, password, local, remote):
    size = os.path.getsize(local)
    temp = remote + ".new"
    ftp = ftplib.FTP(host, timeout=30)
    try:
        ftp.login(user, password)
        ftp.voidcmd("TYPE I")
        with open(local, "rb") as source:
            ftp.storbinary("STOR " + temp, source, blocksize=256 * 1024)
        uploaded = ftp.size(temp)
        if uploaded != size:
            raise RuntimeError("uploaded %s bytes, expected %d" % (uploaded, size))
        try:
            ftp.delete(remote)
        except ftplib.error_perm:
            pass  # nothing there yet
        ftp.rename(temp, remote)
        if ftp.size(remote) != size:
            raise RuntimeError("size changed after rename")
    finally:
        try:
            ftp.quit()
        except Exception:
            ftp.close()
    print("Deployed %s (%d bytes) to %s:%s" % (local, size, host, remote))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="192.168.68.71")
    parser.add_argument("--user", default="xboxftp")
    parser.add_argument("--password", default="xboxftp")
    parser.add_argument("--local", default="Release/Omni360.xex")
    parser.add_argument("--remote", default="/Game/Omni360.xex")
    args = parser.parse_args()
    try:
        deploy(args.host, args.user, args.password, args.local, args.remote)
    except Exception as error:
        print("Deploy failed: %s" % error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
