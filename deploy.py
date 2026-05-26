"""
deploy.py
Deploys SurgeXT Standalone to an RPi target via SFTP (binary) and rsync (data).

    python deploy.py --target-ip 192.168.1.100 --dist build/dist-linux
    python deploy.py --target-ip 192.168.1.100 --dist build/dist-qnx
    python deploy.py --target-ip 192.168.1.100 --dist build/dist-linux --with-data

NOTE: stop any running SurgeXT processes on the target before deploying.

Portable layout on target:
    ~/surge/
      SurgeXT              ← binary
      SurgeXTData/         ← factory data (patches, wavetables, skins)
        patches_factory/
        wavetables/
        ...

After deploy, on the Pi:
    ~/surge/SurgeXT
"""

import os
import getpass
import argparse
import subprocess

try:
    import paramiko
except ImportError:
    raise SystemExit("pip install paramiko")

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_DATA_SRC   = os.path.join(_SCRIPT_DIR, "resources", "data")

def main():
    parser = argparse.ArgumentParser(description="Deploy SurgeXT Standalone to RPi")
    parser.add_argument("--target-ip",   required=True,
                        help="Target IP or hostname (e.g. 192.168.1.100 or user@192.168.1.100)")
    parser.add_argument("--dist",        default=os.path.join(_SCRIPT_DIR, "build", "dist-linux"),
                        help="Dist directory produced by build.sh (default: build/dist-linux)")
    parser.add_argument("--user",        default="root",
                        help="SSH username (default: root)")
    parser.add_argument("--deploy-path", default="~/surge",
                        help="Destination directory on target (default: ~/surge)")
    parser.add_argument("--with-data",   action="store_true",
                        help="Also sync factory data (~490 MB, first time only)")
    args = parser.parse_args()

    binary = os.path.join(args.dist, "SurgeXT")
    if not os.path.exists(binary):
        raise SystemExit(f"Binary not found: {binary}\nRun  ./build.sh [--target linux|qnx]  first.")

    user, _, host = args.target_ip.rpartition('@')
    user = user or args.user
    password = getpass.getpass(f"Password for {user}@{host}: ")

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username=user, password=password)

    deploy_dir = args.deploy_path
    if '~' in deploy_dir:
        _, stdout, _ = client.exec_command('echo $HOME')
        home = stdout.read().decode().strip()
        deploy_dir = deploy_dir.replace('~', home)

    client.exec_command(f'mkdir -p "{deploy_dir}"')

    sftp = client.open_sftp()

    remote_bin = deploy_dir.rstrip('/') + '/SurgeXT'
    print(f"  → SurgeXT")
    sftp.put(binary, remote_bin)
    sftp.chmod(remote_bin, 0o755)

    sftp.close()

    if args.with_data:
        if not os.path.isdir(_DATA_SRC):
            print(f"WARNING: factory data not found at {_DATA_SRC} — skipping --with-data")
        else:
            data_dest = deploy_dir.rstrip('/') + '/SurgeXTData/'
            remote_target = f"{user}@{host}:{data_dest}"
            print(f"\n  Syncing factory data to {remote_target} ...")
            print(f"  (rsync via SSH — this may take a few minutes on first run)")
            # rsync trailing slash on source: copies contents, not the dir itself.
            subprocess.run(
                ["rsync", "-avz", "--progress",
                 f"--rsh=ssh -o StrictHostKeyChecking=no",
                 _DATA_SRC.rstrip('/') + '/',
                 remote_target],
                check=True)
            print(f"  → SurgeXTData/ synced")

    client.close()

    print(f"\nDeployed to {args.target_ip}:{deploy_dir}")
    print(f"\nTo run on the Pi:")
    print(f"  {deploy_dir}/SurgeXT")

if __name__ == "__main__":
    main()
